/*
 * Copyright 2019, Data61, CSIRO (ABN 41 687 119 230)
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <sel4vm/guest_vm.h>
#include <sel4vm/guest_memory.h>

/**
 * Handle a vm memory fault through searching previously created reservations and invoking the appropriate fault callback
 * @param {vm_t *} vm               A handle to the VM
 * @param {vm_vcpu_t *} vcpu        A handle to the faulting vcpu
 * @param {uintptr_t} addr          Faulting address
 * @param {size_t} size             Size of the faulting region
 * @return                          Fault handling status code: HANDLED, UNHANDLED, RESTART, ERROR
 */
memory_fault_result_t vm_memory_handle_fault(vm_t *vm, vm_vcpu_t *vcpu, uintptr_t addr, size_t size);

/**
 * Map a vm memory reservation - this invokation is performed immediately (mapping is not deferred)
 * @param {vm_t *} vm                                   A handle to the VM
 * @param {vm_memory_reservation_t *} vm_reservation    A handle to the VM reservation being mapped
 * @param {memory_map_iterator_fn} map_iterator         Pointer to the map iterator function for retrieving reservation frames
 * @param {void *} map_cookie                           Cookie to pass onto map iterator
 * @return                                              0 on success, -1 on error
 */
int map_vm_memory_reservation(vm_t *vm, vm_memory_reservation_t *vm_reservation,
                              memory_map_iterator_fn map_iterator, void *map_cookie);
