#include "utils/event_update_gate.h"

#include <stdint.h>
#include <psp2/kernel/processmgr.h>
#include <so_util/so_util.h>

#include "utils/logger.h"

extern so_module so_mod;

#define SYM_MGR_UPDATE  "_ZN5moFlo20CGenericEventManager6UpdateEv"
#define SYM_LOCK        "_ZN5boost11unique_lockINS_5mutexEE4lockEv"
#define SYM_UNLOCK      "_ZN5boost11unique_lockINS_5mutexEED1Ev"
#define SYM_RANGE_INS   "_ZNSt6vectorIN5boost10shared_ptrIN5moFlo16IUpdateableEventEEESaIS4_EE15_M_range_insertIN9__gnu_cxx17__normal_iteratorIPS4_S6_EEEEvSB_T_SC_St20forward_iterator_tag"
#define SYM_DESTROY     "_ZNSt12_Destroy_auxILb0EE9__destroyIPN5boost10shared_ptrIN5moFlo16IUpdateableEventEEEEEvT_S8_"
#define SYM_REMOVE_MANY "_ZN5moFlo4Core6CUtils16VectorRemoveManyIN5boost10shared_ptrINS_16IUpdateableEventEEEEEjRSt6vectorIT_SaIS8_EESB_"

#define MGR_LIVE      4
#define MGR_PEND_ADD  16
#define MGR_PEND_DEL  28

#define EV_LISTENERS  4
#define EV_PENDING    16
#define EV_ELEM       16
#define EV_FLAG       12
#define EVG_MAX_SPAN  4096

#define EVG_PREFETCH_AHEAD 3

typedef struct { void *m; uint8_t owns; } boost_unique_lock;

typedef void     (*lock_fn)(boost_unique_lock *self);
typedef void     (*unlock_fn)(boost_unique_lock *self);
typedef void     (*range_insert_fn)(void *vec, void *pos, void *first, void *last);
typedef void     (*destroy_fn)(void *first, void *last);
typedef uint32_t (*remove_many_fn)(void *vec, void *to_remove);
typedef void     (*ev_update_fn)(void *self);

static lock_fn         g_lock;
static unlock_fn       g_unlock;
static range_insert_fn g_range_insert;
static destroy_fn      g_destroy;
static remove_many_fn  g_remove_many;

#define GATE_ON() (1)

static inline int evg_idle(uint8_t *ev) {
    const uint8_t *pb = *(const uint8_t *volatile *)(ev + EV_PENDING);
    const uint8_t *pe = *(const uint8_t *volatile *)(ev + EV_PENDING + 4);
    if (pb != pe) {
        return 0;
    }

    uint8_t *lb = *(uint8_t *volatile *)(ev + EV_LISTENERS);
    const uint8_t *le = *(const uint8_t *volatile *)(ev + EV_LISTENERS + 4);
    uint32_t span = (uint32_t)(le - lb);
    if (span == 0) return 1;
    if (span > EVG_MAX_SPAN || (span & (EV_ELEM - 1)) != 0) {
        return 0;
    }
    for (uint32_t off = 0; off < span; off += EV_ELEM) {
        if (*(volatile uint8_t *)(lb + off + EV_FLAG)) {
            return 0;
        }
    }
    return 1;
}

static void evg_manager_update(void *self) {
    uint8_t *m = (uint8_t *)self;

    boost_unique_lock ul = { .m = m, .owns = 0 };
    g_lock(&ul);

    g_range_insert(m + MGR_LIVE, *(void **)(m + MGR_LIVE + 4),
                   *(void **)(m + MGR_PEND_ADD), *(void **)(m + MGR_PEND_ADD + 4));
    void *pa_begin = *(void **)(m + MGR_PEND_ADD);
    g_destroy(pa_begin, *(void **)(m + MGR_PEND_ADD + 4));
    *(void **)(m + MGR_PEND_ADD + 4) = pa_begin;

    g_remove_many(m + MGR_LIVE, m + MGR_PEND_DEL);
    void *pd_begin = *(void **)(m + MGR_PEND_DEL);
    g_destroy(pd_begin, *(void **)(m + MGR_PEND_DEL + 4));
    *(void **)(m + MGR_PEND_DEL + 4) = pd_begin;

    for (uint32_t i = 0;; i++) {
        uint8_t *b = *(uint8_t *volatile *)(m + MGR_LIVE);
        const uint8_t *e = *(const uint8_t *volatile *)(m + MGR_LIVE + 4);
        uint32_t n = (uint32_t)((e - b) >> 3);
        if (i >= n) break;

        if (i + EVG_PREFETCH_AHEAD < n) {
            uint8_t *nxt = *(uint8_t **)(b + (i + EVG_PREFETCH_AHEAD) * 8);
            if (nxt) __builtin_prefetch(nxt + EV_LISTENERS, 0, 1);
        }
        uint8_t *ev = *(uint8_t **)(b + i * 8);
        if (!ev) continue;
        if (evg_idle(ev)) continue;
        ((ev_update_fn *)(*(void **)ev))[0](ev);
    }

    g_unlock(&ul);
}

void event_update_gate_install(void) {
    uintptr_t update = so_symbol(&so_mod, SYM_MGR_UPDATE);
    g_lock         = (lock_fn)so_symbol(&so_mod, SYM_LOCK);
    g_unlock       = (unlock_fn)so_symbol(&so_mod, SYM_UNLOCK);
    g_range_insert = (range_insert_fn)so_symbol(&so_mod, SYM_RANGE_INS);
    g_destroy      = (destroy_fn)so_symbol(&so_mod, SYM_DESTROY);
    g_remove_many  = (remove_many_fn)so_symbol(&so_mod, SYM_REMOVE_MANY);

    if (!update || !g_lock || !g_unlock || !g_range_insert || !g_destroy || !g_remove_many) {
        l_error("event-gate: a symbol is missing (update=%p lock=%p unlock=%p "
                "range_insert=%p destroy=%p remove_many=%p) - NOT installed, "
                "the manager walks every event",
                (void *)update, (void *)g_lock, (void *)g_unlock,
                (void *)g_range_insert, (void *)g_destroy, (void *)g_remove_many);
        return;
    }
    (void)hook_addr(update, (uintptr_t)&evg_manager_update);
}
