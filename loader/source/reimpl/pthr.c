/*
 * Copyright (C) 2021      Andy Nguyen
 * Copyright (C) 2022      Rinnegatamante
 * Copyright (C) 2022      GrapheneCt
 * Copyright (C) 2022-2024 Volodymyr Atamanenko
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include "reimpl/pthr.h"

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/processmgr.h>
#include <stdatomic.h>

#include "reimpl/bits/_errno_bionic.h"
#include "utils/utils.h"
#include "utils/logger.h"
#include "utils/watchdog.h"
#include "utils/update_profile.h"

#define BIONIC_PTHREAD_COND_INITIALIZER              0
#define BIONIC_PTHREAD_MUTEX_INITIALIZER             0
#define BIONIC_PTHREAD_RECURSIVE_MUTEX_INITIALIZER   0x4000
#define BIONIC_PTHREAD_ERRORCHECK_MUTEX_INITIALIZER  0x8000

enum {
    BIONIC_PTHREAD_MUTEX_NORMAL = 0,
    BIONIC_PTHREAD_MUTEX_RECURSIVE = 1,
    BIONIC_PTHREAD_MUTEX_ERRORCHECK = 2,

    BIONIC_PTHREAD_MUTEX_ERRORCHECK_NP = BIONIC_PTHREAD_MUTEX_ERRORCHECK,
    BIONIC_PTHREAD_MUTEX_RECURSIVE_NP  = BIONIC_PTHREAD_MUTEX_RECURSIVE,

    BIONIC_PTHREAD_MUTEX_DEFAULT = BIONIC_PTHREAD_MUTEX_NORMAL
};

#define PTHR_INLINE static inline __attribute__((always_inline))

#define PTHR_STACK_SIZE (512 * 1024)

#define PTHR_TABLE_INITIAL_CAPACITY 4096
#define PTHR_TOMBSTONE ((void *)(uintptr_t)1)

typedef struct pthr_table {
    uint32_t mask;
    uint32_t count;
    uint32_t used;
    _Atomic(void *) slots[];
} pthr_table;

static _Atomic(pthr_table *) pthr_objects = NULL;

static SceKernelLwMutexWork pthr_mutex;

#define PTHR_SHARDS 16
#define PTHR_SEMA_CACHE_MAX 32

static SceKernelLwMutexWork pthr_shards[PTHR_SHARDS];

static SceKernelLwMutexWork pthr_sema_lock;
static SceUID pthr_sema_cache[PTHR_SEMA_CACHE_MAX];
static int    pthr_sema_cached = 0;

static _Atomic int pthr_mutex_state = 0;
static atomic_flag pthr_mutex_creating = ATOMIC_FLAG_INIT;

void pthr_init(void) {
    if (atomic_load_explicit(&pthr_mutex_state, memory_order_acquire) != 0)
        return;

    if (atomic_flag_test_and_set_explicit(&pthr_mutex_creating,
                                          memory_order_acq_rel)) {
        while (atomic_load_explicit(&pthr_mutex_state, memory_order_acquire) == 0)
            sceKernelDelayThread(100);
        return;
    }

    int ret = sceKernelCreateLwMutex(&pthr_mutex, "pthr_objects", 0, 0, NULL);
    if (ret < 0) {
        l_error("failed to create pthr lock: 0x%x", ret);
        atomic_store_explicit(&pthr_mutex_state, -1, memory_order_release);
        return;
    }

    for (int i = 0; i < PTHR_SHARDS; ++i) {
        ret = sceKernelCreateLwMutex(&pthr_shards[i], "pthr_shard", 0, 0, NULL);
        if (ret < 0) {
            l_error("failed to create pthr shard lock %d: 0x%x", i, ret);
            atomic_store_explicit(&pthr_mutex_state, -1, memory_order_release);
            return;
        }
    }

    ret = sceKernelCreateLwMutex(&pthr_sema_lock, "pthr_semas", 0, 0, NULL);
    if (ret < 0) {
        l_error("failed to create pthr sema-cache lock: 0x%x", ret);
        atomic_store_explicit(&pthr_mutex_state, -1, memory_order_release);
        return;
    }

    atomic_store_explicit(&pthr_mutex_state, 1, memory_order_release);
}

static int pthr_lock(void) {
    if (atomic_load_explicit(&pthr_mutex_state, memory_order_acquire) != 1) {
        pthr_init();
        if (atomic_load_explicit(&pthr_mutex_state, memory_order_acquire) != 1)
            return -1;
    }
    sceKernelLockLwMutex(&pthr_mutex, 1, NULL);
    return 0;
}

static void pthr_unlock(void) {
    sceKernelUnlockLwMutex(&pthr_mutex, 1);
}

static inline uint32_t pthr_hash(const void * p) {
    uint32_t v = (uint32_t)(uintptr_t)p >> 2;
    return v * 2654435761u;
}

static pthr_table * pthr_table_alloc(uint32_t capacity) {
    pthr_table * t = calloc(1, sizeof(pthr_table) +
                               capacity * sizeof(_Atomic(void *)));
    if (!t) return NULL;
    t->mask = capacity - 1;
    return t;
}

static int pthr_table_contains(pthr_table * t, const void * obj) {
    if (!t) return 0;
    uint32_t mask = t->mask;
    uint32_t i = pthr_hash(obj) & mask;
    for (uint32_t n = 0; n <= mask; ++n) {
        void * s = atomic_load_explicit(&t->slots[i], memory_order_relaxed);
        if (s == obj) return 1;
        if (s == NULL) return 0;
        i = (i + 1) & mask;
    }
    return 0;
}

static void pthr_table_put_locked(pthr_table * t, void * obj) {
    uint32_t i = pthr_hash(obj) & t->mask;
    for (;;) {
        void * s = atomic_load_explicit(&t->slots[i], memory_order_relaxed);
        if (s == NULL || s == PTHR_TOMBSTONE) {
            if (s == NULL) t->used++;
            t->count++;
            atomic_store_explicit(&t->slots[i], obj, memory_order_release);
            return;
        }
        i = (i + 1) & t->mask;
    }
}

static int pthr_table_grow_locked(void) {
    pthr_table * old = atomic_load_explicit(&pthr_objects, memory_order_relaxed);
    uint32_t capacity = old ? (old->mask + 1) * 2 : PTHR_TABLE_INITIAL_CAPACITY;

    pthr_table * new = pthr_table_alloc(capacity);
    if (!new) return 0;

    if (old) {
        for (uint32_t i = 0; i <= old->mask; ++i) {
            void * s = atomic_load_explicit(&old->slots[i], memory_order_relaxed);
            if (s && s != PTHR_TOMBSTONE)
                pthr_table_put_locked(new, s);
        }
        l_debug("pthr object table grown to %u entries (%u live)",
                capacity, new->count);
    }

    atomic_store_explicit(&pthr_objects, new, memory_order_release);
    return 1;
}

static int object_is_initialized_locked(const void * obj) {
    return pthr_table_contains(
        atomic_load_explicit(&pthr_objects, memory_order_relaxed), obj);
}

static int object_is_initialized_fast(const void * obj) {
    pthr_table * t = atomic_load_explicit(&pthr_objects, memory_order_acquire);
    if (pthr_table_contains(t, obj)) {
        atomic_thread_fence(memory_order_acquire);
        return 1;
    }
    return 0;
}

static int object_remember_locked(void * obj) {
    pthr_table * t = atomic_load_explicit(&pthr_objects, memory_order_relaxed);

    if (!t || (t->used + 1) * 4 >= (t->mask + 1) * 3) {
        if (!pthr_table_grow_locked()) return 0;
        t = atomic_load_explicit(&pthr_objects, memory_order_relaxed);
    }

    pthr_table_put_locked(t, obj);
    return 1;
}

int isObjectInitialized(const void * mut) {
    if (pthr_lock() != 0) return 0;
    int ret = object_is_initialized_locked(mut);
    pthr_unlock();
    return ret;
}

int rememberObject(void * mut) {
    if (pthr_lock() != 0) return 0;
    int ret = object_is_initialized_locked(mut) ? 1 : object_remember_locked(mut);
    pthr_unlock();
    return ret;
}

int forgetObject(const void * mut) {
    if (pthr_lock() != 0) return 0;

    pthr_table * t = atomic_load_explicit(&pthr_objects, memory_order_relaxed);
    if (!t) {
        pthr_unlock();
        return 0;
    }

    uint32_t i = pthr_hash(mut) & t->mask;
    for (uint32_t n = 0; n <= t->mask; ++n) {
        void * s = atomic_load_explicit(&t->slots[i], memory_order_relaxed);
        if (s == mut) {
            atomic_store_explicit(&t->slots[i], PTHR_TOMBSTONE,
                                  memory_order_release);
            t->count--;
            pthr_unlock();
            return 1;
        }
        if (s == NULL) break;
        i = (i + 1) & t->mask;
    }

    pthr_unlock();
    return 0;
}

typedef struct pthr_waiter {
    struct pthr_waiter * next;
    SceUID sema;
    SceUID tid;
} pthr_waiter;

typedef struct {
    pthr_waiter * head;
    pthr_waiter * tail;
} pthr_queue;

enum {
    VMUTEX_FREE = 0,
    VMUTEX_HELD = 1,
    VMUTEX_HELD_CONTENDED = 2,
};

typedef struct {
    _Atomic uint32_t state;
    int32_t  kind;
    SceUID   owner;
    uint32_t recursion;
    pthr_queue q;
} vmutex;

#define VMUTEX_OWNER_ELIDED ((SceUID)-1)

typedef struct {
    pthr_queue q;
} vcond;

typedef struct { vmutex m; uint32_t pad[2]; } pthr_arena_slot;
_Static_assert(sizeof(pthr_arena_slot) == 32, "arena slot must stay 32 bytes for the mask check");
#ifndef PTHR_ARENA_SLOTS
#define PTHR_ARENA_SLOTS 16384
#endif
static pthr_arena_slot pthr_arena[PTHR_ARENA_SLOTS];
static uint32_t pthr_arena_fresh = 0;
static uint32_t pthr_arena_free_stack[PTHR_ARENA_SLOTS];
static uint32_t pthr_arena_free_top = 0;
static uint32_t pthr_arena_fallbacks = 0;

static inline int pthr_arena_contains(const void * p) {
    uintptr_t off = (uintptr_t)p - (uintptr_t)pthr_arena;
    return off < sizeof(pthr_arena) && (off & 31u) == 0;
}

static vmutex * pthr_vmutex_alloc_locked(void) {
    pthr_arena_slot * s = NULL;
    if (pthr_arena_free_top) s = &pthr_arena[pthr_arena_free_stack[--pthr_arena_free_top]];
    else if (pthr_arena_fresh < PTHR_ARENA_SLOTS) s = &pthr_arena[pthr_arena_fresh++];
    if (!s) {
        pthr_arena_fallbacks++;
        return calloc(1, sizeof(vmutex));
    }
    memset(s, 0, sizeof(*s));
    return &s->m;
}

static void pthr_vmutex_free_locked(vmutex * v) {
    if (pthr_arena_contains(v)) {
        pthr_arena_free_stack[pthr_arena_free_top++] =
            (uint32_t)((pthr_arena_slot *)v - pthr_arena);
    } else {
        free(v);
    }
}

static inline SceKernelLwMutexWork * shard_of(const void * obj) {
    return &pthr_shards[(pthr_hash(obj) >> 16) & (PTHR_SHARDS - 1)];
}

static inline void shard_lock(SceKernelLwMutexWork * s) {
    sceKernelLockLwMutex(s, 1, NULL);
}

static inline void shard_unlock(SceKernelLwMutexWork * s) {
    sceKernelUnlockLwMutex(s, 1);
}

static SceUID pthr_sema_acquire(void) {
    sceKernelLockLwMutex(&pthr_sema_lock, 1, NULL);
    if (pthr_sema_cached > 0) {
        SceUID s = pthr_sema_cache[--pthr_sema_cached];
        sceKernelUnlockLwMutex(&pthr_sema_lock, 1);
        return s;
    }
    sceKernelUnlockLwMutex(&pthr_sema_lock, 1);

    SceUID s = sceKernelCreateSema("pthr_park", 0, 0, 1, NULL);
    if (s < 0)
        l_error("pthr: out of kernel UIDs parking a thread: 0x%08X", s);
    return s;
}

static void pthr_sema_release(SceUID s) {
    sceKernelLockLwMutex(&pthr_sema_lock, 1, NULL);
    if (pthr_sema_cached < PTHR_SEMA_CACHE_MAX) {
        pthr_sema_cache[pthr_sema_cached++] = s;
        sceKernelUnlockLwMutex(&pthr_sema_lock, 1);
        return;
    }
    sceKernelUnlockLwMutex(&pthr_sema_lock, 1);
    sceKernelDeleteSema(s);
}

static void queue_push(pthr_queue * q, pthr_waiter * w) {
    w->next = NULL;
    if (q->tail) q->tail->next = w;
    else         q->head = w;
    q->tail = w;
}

static pthr_waiter * queue_pop(pthr_queue * q) {
    pthr_waiter * w = q->head;
    if (!w) return NULL;
    q->head = w->next;
    if (!q->head) q->tail = NULL;
    w->next = NULL;
    return w;
}

static int queue_remove(pthr_queue * q, pthr_waiter * w) {
    pthr_waiter ** link = &q->head;
    pthr_waiter * prev = NULL;
    while (*link) {
        if (*link == w) {
            *link = w->next;
            if (q->tail == w) q->tail = prev;
            w->next = NULL;
            return 1;
        }
        prev = *link;
        link = &(*link)->next;
    }
    return 0;
}

static int vmutex_lock(vmutex * m, int try_only) {
    if (m->kind == BIONIC_PTHREAD_MUTEX_NORMAL) {
        uint32_t want = VMUTEX_FREE;
        if (atomic_compare_exchange_strong_explicit(&m->state, &want,
                VMUTEX_HELD, memory_order_acquire, memory_order_relaxed)) {
            m->owner = VMUTEX_OWNER_ELIDED;
            m->recursion = 1;
            return 0;
        }
        if (try_only) return EBUSY;
    }
    SceUID self = sceKernelGetThreadId();

    if (m->kind != BIONIC_PTHREAD_MUTEX_NORMAL && m->owner == self) {
        if (m->kind == BIONIC_PTHREAD_MUTEX_RECURSIVE) {
            m->recursion++;
            return 0;
        }
        return try_only ? EBUSY : EDEADLK_BIONIC;
    }

    uint32_t expected = VMUTEX_FREE;
    if (atomic_compare_exchange_strong_explicit(&m->state, &expected,
            VMUTEX_HELD, memory_order_acquire, memory_order_relaxed)) {
        m->owner = self;
        m->recursion = 1;
        return 0;
    }

    if (try_only) return EBUSY;

    SceKernelLwMutexWork * s = shard_of(m);
    shard_lock(s);

    uint32_t old = atomic_exchange_explicit(&m->state, VMUTEX_HELD_CONTENDED,
                                            memory_order_acquire);
    if (old == VMUTEX_FREE) {
        atomic_store_explicit(&m->state, VMUTEX_HELD, memory_order_relaxed);
        m->owner = self;
        m->recursion = 1;
        shard_unlock(s);
        return 0;
    }

    pthr_waiter w = { .next = NULL, .sema = pthr_sema_acquire(), .tid = self };
    if (w.sema < 0) {
        shard_unlock(s);
        return EAGAIN;
    }

    uint32_t owner_snapshot = (uint32_t)m->owner;

    queue_push(&m->q, &w);
    shard_unlock(s);

    watchdog_note_wait_begin((uint32_t)(uintptr_t)m, (uint32_t)self, owner_snapshot);
    sceKernelWaitSema(w.sema, 1, NULL);
    watchdog_note_wait_end();
    pthr_sema_release(w.sema);

    return 0;
}

static int vmutex_unlock(vmutex * m) {
    if (m->kind != BIONIC_PTHREAD_MUTEX_NORMAL) {
        if (m->owner != sceKernelGetThreadId()) return EPERM;
        if (m->kind == BIONIC_PTHREAD_MUTEX_RECURSIVE && --m->recursion > 0)
            return 0;
    }

    m->owner = 0;
    m->recursion = 0;

    uint32_t expected = VMUTEX_HELD;
    if (atomic_compare_exchange_strong_explicit(&m->state, &expected,
            VMUTEX_FREE, memory_order_release, memory_order_relaxed))
        return 0;

    SceKernelLwMutexWork * s = shard_of(m);
    shard_lock(s);

    pthr_waiter * w = queue_pop(&m->q);
    if (w) {
        m->owner = w->tid;
        m->recursion = 1;
        atomic_store_explicit(&m->state,
                              m->q.head ? VMUTEX_HELD_CONTENDED : VMUTEX_HELD,
                              memory_order_release);
        shard_unlock(s);
        sceKernelSignalSema(w->sema, 1);
    } else {
        atomic_store_explicit(&m->state, VMUTEX_FREE, memory_order_release);
        shard_unlock(s);
    }
    return 0;
}

static int vcond_wait(vcond * c, vmutex * m, SceUInt timeout_us) {
    SceUID sema = pthr_sema_acquire();
    if (sema < 0) return EAGAIN;

    SceUID self = sceKernelGetThreadId();
    pthr_waiter w = { .next = NULL, .sema = sema, .tid = self };

    SceKernelLwMutexWork * s = shard_of(c);
    shard_lock(s);
    queue_push(&c->q, &w);
    shard_unlock(s);

    uint32_t saved_recursion = m->recursion;
    m->recursion = 1;
    vmutex_unlock(m);

    int timed_out = 0;
    if (sceKernelWaitSema(w.sema, 1, timeout_us ? &timeout_us : NULL) < 0) {
        shard_lock(s);
        timed_out = queue_remove(&c->q, &w);
        shard_unlock(s);
        if (!timed_out)
            sceKernelWaitSema(w.sema, 1, NULL);
    }

    pthr_sema_release(w.sema);

    vmutex_lock(m, 0);
    m->recursion = saved_recursion;

    return timed_out ? ETIMEDOUT_BIONIC : 0;
}

static void vcond_wake(vcond * c, int all) {
    SceKernelLwMutexWork * s = shard_of(c);

    shard_lock(s);
    pthr_waiter * woken;
    if (all) {
        woken = c->q.head;
        c->q.head = c->q.tail = NULL;
    } else {
        woken = queue_pop(&c->q);
    }
    shard_unlock(s);

    while (woken) {
        pthr_waiter * next = woken->next;
        sceKernelSignalSema(woken->sema, 1);
        woken = next;
    }
}

PTHR_INLINE int _attr_t_static_init(pthread_attr_t_bionic * attr) {
    if (attr->magic == 0x42424242) return 0;

    pthread_attr_t * real = malloc(sizeof(pthread_attr_t));
    if (!real) return ENOMEM;

    int ret = pthread_attr_init(real);
    if (ret != 0) {
        free(real);
        return ret;
    }

    attr->real_ptr = real;
    atomic_thread_fence(memory_order_release);
    attr->magic = 0x42424242;

    return 0;
}

PTHR_INLINE int _mutex_t_static_init(pthread_mutex_t_bionic * mutex, const pthread_mutexattr_t * attr) {
    int kind = BIONIC_PTHREAD_MUTEX_NORMAL;

    if (object_is_initialized_fast(mutex)) return 0;

    if (pthr_lock() != 0) return EAGAIN;

    if (object_is_initialized_locked(mutex)) {
        pthr_unlock();
        return 0;
    }

    if (attr) {
        int pte_kind = PTHREAD_MUTEX_NORMAL;
        pthread_mutexattr_gettype((pthread_mutexattr_t *) attr, &pte_kind);
        if (pte_kind == PTHREAD_MUTEX_RECURSIVE)
            kind = BIONIC_PTHREAD_MUTEX_RECURSIVE;
        else if (pte_kind == PTHREAD_MUTEX_ERRORCHECK)
            kind = BIONIC_PTHREAD_MUTEX_ERRORCHECK;
    } else {
        if (* (int *) mutex == BIONIC_PTHREAD_RECURSIVE_MUTEX_INITIALIZER) kind = BIONIC_PTHREAD_MUTEX_RECURSIVE;
        else if (* (int *) mutex == BIONIC_PTHREAD_ERRORCHECK_MUTEX_INITIALIZER) kind = BIONIC_PTHREAD_MUTEX_ERRORCHECK;
    }

    vmutex * real = pthr_vmutex_alloc_locked();
    if (!real) {
        pthr_unlock();
        l_error("out of memory initializing mutex %p", mutex);
        return ENOMEM;
    }
    real->kind = kind;

    atomic_store_explicit((_Atomic(void *) *)&mutex->real_ptr, real, memory_order_release);

    if (!object_remember_locked(mutex)) {
        mutex->real_ptr = NULL;
        pthr_vmutex_free_locked(real);
        pthr_unlock();
        l_error("out of memory growing pthr object table, cannot track mutex %p",
                mutex);
        return ENOMEM;
    }

    pthr_unlock();
    return 0;
}

PTHR_INLINE int _cond_t_static_init(pthread_cond_t_bionic * cond, const pthread_condattr_t * attr) {
    (void) attr;

    if (object_is_initialized_fast(cond)) return 0;

    if (pthr_lock() != 0) return EAGAIN;

    if (object_is_initialized_locked(cond)) {
        pthr_unlock();
        return 0;
    }

    vcond * real = calloc(1, sizeof(vcond));
    if (!real) {
        pthr_unlock();
        l_error("out of memory initializing cond %p", cond);
        return ENOMEM;
    }

    cond->real_ptr = real;

    if (!object_remember_locked(cond)) {
        cond->real_ptr = NULL;
        free(real);
        pthr_unlock();
        l_error("out of memory growing pthr object table, cannot track cond %p",
                cond);
        return ENOMEM;
    }

    pthr_unlock();
    return 0;
}

int pthread_create_soloader(pthread_t *thread, const pthread_attr_t_bionic *attr, void *(*start)(void *), void *param) {
    int ret;
    size_t stack_size = PTHR_STACK_SIZE;

    if (!attr) {
        pthread_attr_t a;
        pthread_attr_init(&a);
        pthread_attr_setstacksize(&a, PTHR_STACK_SIZE);
        ret = pthread_create(thread, &a, start, param);
        pthread_attr_destroy(&a);
    } else{
        ret = _attr_t_static_init((pthread_attr_t_bionic *) attr);
        if (ret) return ret;

        size_t requested = 0;
        pthread_attr_getstacksize(attr->real_ptr, &requested);
        if (requested == 0) {
            requested = PTHR_STACK_SIZE;
            pthread_attr_setstacksize(attr->real_ptr, requested);
        }

        stack_size = requested;
        ret = pthread_create(thread, attr->real_ptr, start, param);
    }

    l_debug("pthread_create: start=%p attr=%p stack=%u thread=%p ret=%d",
            start, (void *)attr, (unsigned)stack_size,
            thread ? (void*)*thread : NULL, ret);

    if (ret != 0) log_free_memory("pthread_create failed");

    return ret;
}

int pthread_mutexattr_init_soloader(pthread_mutexattr_t *attr)
{
    return pthread_mutexattr_init(attr);
}

int pthread_mutexattr_settype_soloader(pthread_mutexattr_t *attr, int type)
{
    return pthread_mutexattr_settype(attr, type);
}

int pthread_mutexattr_destroy_soloader(pthread_mutexattr_t *attr)
{
    return pthread_mutexattr_destroy(attr);
}

int pthread_kill_soloader(pthread_t thread, int sig)
{
    return pthread_kill(thread, sig);
}

int pthread_mutex_init_soloader(pthread_mutex_t_bionic *uid, const pthread_mutexattr_t *attr)
{
    if (!uid) return EINVAL;
    return _mutex_t_static_init(uid, attr);
}

int pthread_mutex_destroy_soloader(pthread_mutex_t_bionic *mutex)
{
    if (!mutex) return 0;

    if (!forgetObject(mutex)) return 0;

    if (!mutex->real_ptr) return 0;

    {
        vmutex * v = mutex->real_ptr;
        mutex->real_ptr = 0x0;
        if (pthr_lock() == 0) {
            pthr_vmutex_free_locked(v);
            pthr_unlock();
        } else if (!pthr_arena_contains(v)) {
            free(v);
        }
    }
    return 0;
}

int pthread_mutex_lock_soloader(pthread_mutex_t_bionic *mutex)
{
    if (!mutex) return EINVAL;
    void * rp = mutex->real_ptr;
    if (pthr_arena_contains(rp)) return vmutex_lock(rp, 0);
    int ret = _mutex_t_static_init(mutex, NULL);
    if (ret) return ret;
    return vmutex_lock(mutex->real_ptr, 0);
}

int pthread_mutex_trylock_soloader(pthread_mutex_t_bionic *mutex)
{
    if (!mutex) return EINVAL;
    void * rp = mutex->real_ptr;
    if (pthr_arena_contains(rp)) return vmutex_lock(rp, 1);
    int ret = _mutex_t_static_init(mutex, NULL);
    if (ret) return ret;
    return vmutex_lock(mutex->real_ptr, 1);
}

int pthread_mutex_unlock_soloader(pthread_mutex_t_bionic *mutex)
{
    if (!mutex) return EINVAL;
    void * rp = mutex->real_ptr;
    if (pthr_arena_contains(rp)) return vmutex_unlock(rp);
    if (!object_is_initialized_fast(mutex)) return EPERM;
    return vmutex_unlock(mutex->real_ptr);
}

int pthread_join_soloader(pthread_t thread, void **value_ptr)
{
    return pthread_join(thread, value_ptr);
}

int pthread_condattr_init_soloader(pthread_condattr_t *attr)
{
    if (!attr) return EINVAL;
    return pthread_condattr_init(attr);
}

int pthread_condattr_destroy_soloader(pthread_condattr_t *attr)
{
    if (!attr) return EINVAL;
    return pthread_condattr_destroy(attr);
}

int pthread_cond_init_soloader(pthread_cond_t_bionic *cond,
                               const pthread_condattr_t *attr)
{
    if (!cond) return EINVAL;

    (void)attr;

    pthread_cond_destroy_soloader(cond);
    cond->real_ptr = NULL;

    return 0;
}

int pthread_cond_destroy_soloader(pthread_cond_t_bionic *cond)
{
    if (!cond) return 0;

    if (!forgetObject(cond)) return 0;

    if (!cond->real_ptr) return 0;

    free(cond->real_ptr);
    cond->real_ptr = 0x0;
    return 0;
}

int pthread_cond_signal_soloader(pthread_cond_t_bionic *cond)
{
    if (!cond) return EINVAL;

    int ret = _cond_t_static_init(cond, NULL);
    if (ret) return ret;

    vcond_wake(cond->real_ptr, 0);
    return 0;
}

int pthread_cond_timedwait_soloader(pthread_cond_t_bionic *cond, pthread_mutex_t_bionic *mutex, struct timespec *abstime)
{
    if (!cond || !mutex) return EINVAL;

    int ret = _cond_t_static_init(cond, NULL);
    if (ret) return ret;
    ret = _mutex_t_static_init(mutex, NULL);
    if (ret) return ret;

    if (!abstime) return vcond_wait(cond->real_ptr, mutex->real_ptr, 0);

    struct timeval now;
    gettimeofday(&now, NULL);
    long long now_us = (long long) now.tv_sec * 1000000LL + now.tv_usec;
    long long deadline_us = (long long) abstime->tv_sec * 1000000LL
                          + abstime->tv_nsec / 1000;
    long long rel_us = deadline_us - now_us;

    if (rel_us < 1) rel_us = 1;
    if (rel_us > 0xFFFFFFFELL) rel_us = 0xFFFFFFFELL;

    return vcond_wait(cond->real_ptr, mutex->real_ptr, (SceUInt) rel_us);
}

int pthread_cond_wait_soloader(pthread_cond_t_bionic *cond, pthread_mutex_t_bionic *mutex)
{
    if (!cond || !mutex) return EINVAL;

    int ret = _cond_t_static_init(cond, NULL);
    if (ret) return ret;
    ret = _mutex_t_static_init(mutex, NULL);
    if (ret) return ret;

    return vcond_wait(cond->real_ptr, mutex->real_ptr, 0);
}

int pthread_cond_broadcast_soloader(pthread_cond_t_bionic *cond)
{
    if (!cond) return EINVAL;

    int ret = _cond_t_static_init(cond, NULL);
    if (ret) return ret;

    vcond_wake(cond->real_ptr, 1);
    return 0;
}

int pthread_attr_init_soloader(pthread_attr_t_bionic *attr)
{
    if (!attr) return EINVAL;

    attr->magic = 0;

    return _attr_t_static_init(attr);
}

int pthread_attr_destroy_soloader(pthread_attr_t_bionic *attr)
{
    if (!attr) return 0;
    if (attr->magic != 0x42424242) return 0;

    int ret = pthread_attr_destroy(attr->real_ptr);
    free(attr->real_ptr);
    attr->magic = 0x0;

    return ret;
}

int pthread_attr_setdetachstate_soloader(pthread_attr_t_bionic *attr, int state)
{
    if (!attr) return -1;
    int ret = _attr_t_static_init(attr);
    if (ret) return ret;
    state = !state;
    return pthread_attr_setdetachstate(attr->real_ptr, state);
}

int pthread_attr_setstacksize_soloader(pthread_attr_t_bionic *attr, size_t stacksize) {
    if (!attr) return -1;
    int ret = _attr_t_static_init(attr);
    if (ret) return ret;
    return pthread_attr_setstacksize(attr->real_ptr, stacksize);
}

int pthread_attr_setschedpolicy_soloader(void *attr_v, int policy) {
    l_warn("[pthr-diag] pthread_attr_setschedpolicy(attr=%p, policy=%d) -- no-op, dropped",
           attr_v, policy);
    return 0;
}

int pthread_attr_setschedparam_soloader(void *attr_v, const struct sched_param *param) {
    l_warn("[pthr-diag] pthread_attr_setschedparam(attr=%p, priority=%d) -- no-op, dropped",
           attr_v, param ? param->sched_priority : -1);
    return 0;
}

int pthread_setschedparam_soloader(pthread_t thread, int policy,
                                   const struct sched_param *param)
{
   l_warn("[pthr-diag] pthread_setschedparam(thread=%p, policy=%d, priority=%d)",
          (void*)thread, policy, param ? param->sched_priority : -1);
   return pthread_setschedparam(thread, policy, param);
}

int pthread_getschedparam_soloader(pthread_t thread, int *policy,
                                   struct sched_param *param)
{
    return pthread_getschedparam(thread, policy, param);
}

int pthread_detach_soloader(pthread_t thread)
{
    return pthread_detach(thread);
}

int pthread_equal_soloader(const pthread_t t1, const pthread_t t2)
{
    if (t1 == t2)
        return 1;
    if (!t1 || !t2)
        return 0;
    return pthread_equal(t1, t2);
}

pthread_t pthread_self_soloader()
{
    return pthread_self();
}

int pthread_once_soloader(volatile int *once_control, void (*init_routine)(void)) {
    if (!once_control || !init_routine)
        return -1;

    _Atomic int * ctl = (_Atomic int *) once_control;

    int expected = 0;
    if (atomic_compare_exchange_strong_explicit(ctl, &expected, 1,
                                                memory_order_acq_rel,
                                                memory_order_acquire)) {
        (*init_routine)();
        atomic_store_explicit(ctl, 2, memory_order_release);
        return 0;
    }

    while (atomic_load_explicit(ctl, memory_order_acquire) != 2)
        sceKernelDelayThread(100);

    return 0;
}

#ifndef MAX_TASK_COMM_LEN
#define MAX_TASK_COMM_LEN 16
#endif

int pthread_setname_np_soloader(pthread_t thread, const char* thread_name) {
    if (thread == 0 || thread_name == NULL) {
        return EINVAL;
    }
    size_t thread_name_len = strlen(thread_name);
    if (thread_name_len >= MAX_TASK_COMM_LEN) {
        return ERANGE;
    }

    l_debug("PTHREAD: pthread_setname_np with name %s for thread:0x%x", thread_name, pthread_self());

    return 0;
}

int sem_destroy_soloader(int * uid) {
    if (sceKernelDeleteSema(*uid) < 0)
        return -1;
    return 0;
}

int sem_init_soloader (int * uid, int pshared, unsigned int value) {
    *uid = sceKernelCreateSema("sema", 0, (int) value, 0x7fffffff, NULL);
    if (*uid < 0)
        return -1;
    return 0;
}

int sem_post_soloader (int * uid) {
    if (sceKernelSignalSema(*uid, 1) < 0)
        return -1;
    return 0;
}

int sem_wait_soloader (int * uid) {
    if (sceKernelWaitSema(*uid, 1, NULL) < 0)
        return -1;
    return 0;
}
