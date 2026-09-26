/*
 * Copyright (C) 2022-2024 Volodymyr Atamanenko
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#ifndef SOLOADER_LAZY_LWMUTEX_H
#define SOLOADER_LAZY_LWMUTEX_H

#ifdef __cplusplus
extern "C" {
#endif

#include <psp2/kernel/threadmgr.h>
#include <stdatomic.h>
#include <stdbool.h>

typedef struct {
    SceKernelLwMutexWork work;
    _Atomic int state;
    atomic_flag creating;
} lazy_lwmutex_t;

#define LAZY_LWMUTEX_INITIALIZER { .state = 0, .creating = ATOMIC_FLAG_INIT }

static inline bool lazy_lwmutex_lock(lazy_lwmutex_t * m, const char * name) {
    if (atomic_load_explicit(&m->state, memory_order_acquire) == 0) {
        if (atomic_flag_test_and_set_explicit(&m->creating, memory_order_acq_rel)) {
            while (atomic_load_explicit(&m->state, memory_order_acquire) == 0)
                sceKernelDelayThread(100);
        } else if (sceKernelCreateLwMutex(&m->work, name, 0, 0, NULL) < 0) {
            atomic_store_explicit(&m->state, -1, memory_order_release);
        } else {
            atomic_store_explicit(&m->state, 1, memory_order_release);
        }
    }
    if (atomic_load_explicit(&m->state, memory_order_acquire) != 1)
        return false;
    sceKernelLockLwMutex(&m->work, 1, NULL);
    return true;
}

static inline void lazy_lwmutex_unlock(lazy_lwmutex_t * m) {
    sceKernelUnlockLwMutex(&m->work, 1);
}

#ifdef __cplusplus
};
#endif

#endif
