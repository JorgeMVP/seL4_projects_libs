/*
 * Copyright 2019, Data61
 * Commonwealth Scientific and Industrial Research Organisation (CSIRO)
 * ABN 41 687 119 230.
 *
 * This software may be distributed and modified according to the terms of
 * the GNU General Public License version 2. Note that NO WARRANTY is provided.
 * See "LICENSE_GPLv2.txt" for details.
 *
 * @TAG(DATA61_GPL)
 */

/*vm exits related with io instructions*/

#include <stdio.h>
#include <stdlib.h>

#include <sel4/sel4.h>
#include <sel4utils/util.h>
#include <simple/simple.h>

#include <sel4vm/guest_vm.h>
#include <sel4vm/ioports.h>

#include "sel4vm/debug.h"
#include "sel4vm/vmm.h"

#include "vm.h"

static int io_port_compare_by_range(const void *pkey, const void *pelem)
{
    unsigned int key = (unsigned int)pkey;
    const vm_ioport_entry_t *entry = (const vm_ioport_entry_t *)pelem;
    const vm_ioport_range_t *elem = &entry->range;
    if (key < elem->start) {
        return -1;
    }
    if (key > elem->end) {
        return 1;
    }
    return 0;
}

static int io_port_compare_by_start(const void *a, const void *b)
{
    const vm_ioport_entry_t *a_entry = (const vm_ioport_entry_t *)a;
    const vm_ioport_range_t *a_range = &a_entry->range;
    const vm_ioport_entry_t *b_entry = (const vm_ioport_entry_t *)b;
    const vm_ioport_range_t *b_range = &b_entry->range;
    return a_range->start - b_range->start;
}

static vm_ioport_entry_t *search_port(vm_io_port_list_t *ioports, unsigned int port_no)
{
    return (vm_ioport_entry_t *)bsearch((void *)(uintptr_t)port_no, ioports->ioports, ioports->num_ioports,
                                     sizeof(vm_ioport_entry_t), io_port_compare_by_range);
}

static void set_io_in_unhandled(vm_vcpu_t *vcpu, unsigned int size) {
    uint32_t eax;
    if (size < 4) {
        eax = vmm_read_user_context(&vcpu->vcpu_arch.guest_state, USER_CONTEXT_EAX);
        eax |= MASK(size * 8);
    } else {
        eax = -1;
    }
    vmm_set_user_context(&vcpu->vcpu_arch.guest_state, USER_CONTEXT_EAX, eax);
}

static void set_io_in_value(vm_vcpu_t *vcpu, unsigned int value, unsigned int size) {
    uint32_t eax;
    if (size < 4) {
        eax = vmm_read_user_context(&vcpu->vcpu_arch.guest_state, USER_CONTEXT_EAX);
        eax &= ~MASK(size * 8);
        eax |= value;
    } else {
        eax = value;
    }
    vmm_set_user_context(&vcpu->vcpu_arch.guest_state, USER_CONTEXT_EAX, eax);
}

static int add_io_port_range(vm_io_port_list_t *ioport_list, vm_ioport_entry_t port)
{
    /* ensure this range does not overlap */
    for (int i = 0; i < ioport_list->num_ioports; i++) {
        if (ioport_list->ioports[i].range.end > port.range.start && ioport_list->ioports[i].range.start < port.range.end) {
            ZF_LOGE("Requested ioport range 0x%x-0x%x for %s overlaps with existing range 0x%x-0x%x for %s",
                    port.range.start, port.range.end, port.interface.desc ? port.interface.desc : "Unknown IO Port",
                    ioport_list->ioports[i].range.start, ioport_list->ioports[i].range.end,
                    ioport_list->ioports[i].interface.desc ? ioport_list->ioports[i].interface.desc : "Unknown IO Port");
            return -1;
        }
    }
    /* grow the array */
    ioport_list->ioports = realloc(ioport_list->ioports, sizeof(vm_ioport_entry_t) * (ioport_list->num_ioports + 1));
    assert(ioport_list->ioports);
    /* add the new entry */
    ioport_list->ioports[ioport_list->num_ioports] = port;
    ioport_list->num_ioports++;
    /* sort */
    qsort(ioport_list->ioports, ioport_list->num_ioports, sizeof(vm_ioport_entry_t), io_port_compare_by_start);
    return 0;
}

int vm_enable_passthrough_ioport(vm_vcpu_t *vcpu, uint16_t port_start, uint16_t port_end) {
    cspacepath_t path;
    int error;
    ZF_LOGD("Enabling IO port 0x%x - 0x%x for passthrough", port_start, port_end);
    error = vka_cspace_alloc_path(vcpu->vm->vka, &path);
    if (error) {
        ZF_LOGE("Failed to allocate slot");
        return error;
    }
    error = simple_get_IOPort_cap(vcpu->vm->simple, port_start, port_end, path.root, path.capPtr, path.capDepth);
    if (error) {
        ZF_LOGE("Failed to get io port from simple for range 0x%x - 0x%x", port_start, port_end);
        return error;
    }
    error = seL4_X86_VCPU_EnableIOPort(vcpu->vcpu.cptr, path.capPtr, port_start, port_end);
    if (error != seL4_NoError) {
        ZF_LOGE("Failed to enable io port");
        return error;
    }
    return 0;
}

/* IO instruction execution handler. */
int vmm_io_instruction_handler(vm_vcpu_t *vcpu) {

    unsigned int exit_qualification = vmm_guest_exit_get_qualification(&vcpu->vcpu_arch.guest_state);
    unsigned int string, rep;
    int ret;
    unsigned int port_no;
    unsigned int size;
    unsigned int value;
    int is_in;

    string = (exit_qualification & 16) != 0;
    is_in = (exit_qualification & 8) != 0;
    port_no = exit_qualification >> 16;
    size = (exit_qualification & 7) + 1;
    rep = (exit_qualification & 0x20) >> 5;

    /*FIXME: does not support string and rep instructions*/
    if (string || rep) {
        ZF_LOGE("FIXME: IO exit does not support string and rep instructions");
        return VM_EXIT_HANDLE_ERROR;
    }

    if (!vcpu->vm->arch.ioport_callback) {
        if (port_no != -1) {
            ZF_LOGW("ignoring unsupported ioport 0x%x", port_no);
        }
        if (is_in) {
            set_io_in_unhandled(vcpu, size);
        }
        vmm_guest_exit_next_instruction(&vcpu->vcpu_arch.guest_state, vcpu->vcpu.cptr);
        return VM_EXIT_HANDLED;
    }

    if (!is_in) {
        value = vmm_read_user_context(&vcpu->vcpu_arch.guest_state, USER_CONTEXT_EAX);
        if (size < 4) {
            value &= MASK(size * 8);
        }
    }
    ioport_fault_result_t res = vcpu->vm->arch.ioport_callback(vcpu->vm, port_no, is_in, &value, size, vcpu->vm->arch.ioport_callback_cookie);
    if (is_in) {
        if (res == IO_FAULT_UNHANDLED) {
            set_io_in_unhandled(vcpu, size);
        } else {
            set_io_in_value(vcpu, value, size);
        }
    }

    if (res == IO_FAULT_ERROR) {
        ZF_LOGE("VM Exit IO Error: string %d  in %d rep %d  port no 0x%x size %d", 0,
                is_in, 0, port_no, size);
        return VM_EXIT_HANDLE_ERROR;
    }

    vmm_guest_exit_next_instruction(&vcpu->vcpu_arch.guest_state, vcpu->vcpu.cptr);

    return VM_EXIT_HANDLED;
}

int vm_register_unhandled_ioport_callback(vm_t *vm, unhandled_ioport_callback_fn ioport_callback,
                                      void *cookie) {
    if (!vm) {
        ZF_LOGE("Failed to register ioport callback: Invalid VM handle");
        return -1;
    }

    if (!ioport_callback) {
        ZF_LOGE("Failed to register ioport callback: Invalid callback");
        return -1;
    }
    vm->arch.unhandled_ioport_callback = ioport_callback;
    vm->arch.unhandled_ioport_callback_cookie = cookie;
    return 0;
}

int vm_io_port_add_handler(vm_t *vm, vm_ioport_range_t io_range, vm_ioport_interface_t io_interface)
{
    return add_io_port_range(&vm->arch.ioport_list, (vm_ioport_entry_t) {io_range, io_interface});
}
