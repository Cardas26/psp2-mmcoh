#include "utils/render_command_cache.h"

#include <string.h>
#include <so_util/so_util.h>

#include "utils/logger.h"

extern so_module so_mod;

#define DSB_BATCH(idx)      (0x00 + 4 * (idx))
#define DSB_SPR_BEGIN       0x08
#define DSB_SPR_END         0x0c
#define DSB_CMD_BEGIN       0x14
#define DSB_CMD_END         0x18
#define DSB_CMD_CAP         0x1c
#define DSB_LAST_MAT_PX     0x20
#define DSB_BUF_INDEX       0x28
#define DSB_SPRITES_IN_CMD  0x2c

#define SPRITE_DATA_SIZE    120
#define RENDER_CMD_SIZE     0x184
#define CMD_OFFSET          0x17c
#define CMD_COUNT           0x180

static const uint8_t k_map_value_size[MAT_MAP_COUNT] = MAT_MAP_VALUE_SIZES;
#define MAP_KEY_MAX         32

#define SYM_FORCE_CHANGE    "_ZN5moFlo9Rendering19CDynamicSpriteBatch18ForceCommandChangeEv"
#define SYM_FLUSH           "_ZN5moFlo9Rendering19CDynamicSpriteBatch18BuildAndFlushBatchEPNS0_13IRenderSystemE"
#define SYM_DSB_DTOR        "_ZN5moFlo9Rendering19CDynamicSpriteBatchD1Ev"
#define SYM_MAT_CTOR        "_ZN5moFlo9Rendering9CMaterialC1Ev"
#define SYM_MAT_DTOR        "_ZN5moFlo9Rendering9CMaterialD1Ev"
#define SYM_MAT_ASSIGN      "_ZN5moFlo9Rendering9CMaterialaSERKS1_"
#define SYM_BATCH_BUILD     "_ZN5moFlo9Rendering12CSpriteBatch5BuildEPSt6vectorINS0_16CSpriteComponent10SpriteDataESaIS4_EE"
#define SYM_BATCH_RENDER    "_ZNK5moFlo9Rendering12CSpriteBatch6RenderEPNS0_13IRenderSystemERKNS0_9CMaterialEjj"
#define SYM_DESTROY_SPRITES "_ZNSt12_Destroy_auxILb0EE9__destroyIPN5moFlo9Rendering16CSpriteComponent10SpriteDataEEEvT_S7_"

typedef void (*mat_ctor_fn)(void * mat);
typedef void (*mat_dtor_fn)(void * mat);
typedef void (*mat_assign_fn)(void * dst, const void * src);
typedef void (*batch_build_fn)(void * batch, void * sprite_vector);
typedef void (*batch_render_fn)(void * batch, void * rs, const void * material, uint32_t offset, uint32_t count);
typedef void (*destroy_sprites_fn)(void * begin, void * end);

static mat_ctor_fn        g_mat_ctor;
static mat_dtor_fn        g_mat_dtor;
static mat_assign_fn      g_mat_assign;
static batch_build_fn     g_batch_build;
static batch_render_fn    g_batch_render;
static destroy_sprites_fn g_destroy_sprites;

static so_hook g_force_hook;
static so_hook g_flush_hook;
static so_hook g_dtor_hook;

#define MAX_INSTANCES 2
#define POOL_SIZE     256
#define MAX_CMDS      2048
#define MAX_TEXTURES  8
#define NAME_MAX      48
#define EVICT_AFTER   240

typedef struct {
    const uint8_t * src;
    uint32_t        last_flush;
    uint8_t         cmd[RENDER_CMD_SIZE];
    uint8_t         snap[MAT_SIZE];
    uint32_t        tex[MAX_TEXTURES * 2];
    uint32_t        ntex;
    char            name[NAME_MAX];
    uint8_t         maps[MAP_SNAP_MAX];
    uint16_t        maps_len;
    uint8_t         maps_ok;
} pool_t;

typedef struct {
    pool_t * p;
    uint32_t offset;
    uint32_t count;
} cmd_t;

typedef struct {
    uint8_t * self;
    uint32_t  ncmds;
    cmd_t     cmds[MAX_CMDS];
} inst_t;

static pool_t   g_pool[POOL_SIZE];
static uint32_t g_pool_live;
static inst_t   g_inst[MAX_INSTANCES];
static uint32_t g_flush_no;

static uint32_t c_changes, c_skipped, c_assigned, c_constructed, c_evicted, c_dropped, c_flushes;

enum { R_SAME = 0, R_FRESH, R_BYTES, R_MAPS, R_TEX, R_NAME, R_COUNT };
static uint32_t c_why[R_COUNT];

#define g_gate_on 1

#define g_deep_on 1

static inst_t * inst_for(uint8_t * self) {
    inst_t * free_row = NULL;
    for (int i = 0; i < MAX_INSTANCES; i++) {
        if (g_inst[i].self == self) return &g_inst[i];
        if (!g_inst[i].self && !free_row) free_row = &g_inst[i];
    }
    if (free_row) {
        free_row->self = self;
        free_row->ncmds = 0;
        return free_row;
    }
    return NULL;
}

static void pool_destroy(pool_t * e) {
    if (!e->src) return;
    g_mat_dtor(e->cmd);
    e->src = NULL;
    g_pool_live--;
    c_evicted++;
}

static int material_diff(const uint8_t * src, const pool_t * e);

static pool_t * pool_get(const uint8_t * src, int * fresh) {
    uint32_t h = ((uintptr_t)src >> 4) & (POOL_SIZE - 1);
    pool_t * victim = NULL;
    *fresh = 0;
    for (uint32_t k = 0; k < POOL_SIZE; k++) {
        pool_t * e = &g_pool[(h + k) & (POOL_SIZE - 1)];
        if (e->src == src) {
            if (e->last_flush != g_flush_no || material_diff(src, e) == R_SAME)
                return e;
            continue;
        }
        if (!e->src) { victim = e; break; }
        if (e->last_flush == g_flush_no)
            continue;
        if (!victim || e->last_flush < victim->last_flush) victim = e;
    }
    if (!victim) return NULL;
    if (victim->src)
        pool_destroy(victim);
    memset(victim->cmd, 0, RENDER_CMD_SIZE);
    g_mat_ctor(victim->cmd);
    victim->src = src;
    victim->ntex = 0;
    victim->name[0] = 0;
    g_pool_live++;
    c_constructed++;
    *fresh = 1;
    return victim;
}

int rcc_maps_serialise(const uint8_t * src, uint8_t * buf) {
    int len = 0;
    for (int m = 0; m < MAT_MAP_COUNT; m++) {
        const uint8_t * map = src + MAT_MAP_FIRST + m * MAT_MAP_STRIDE;
        if (*(const uint32_t *)(map + MAT_MAP_SIZE_OFF) == 0)
            continue;
        const uint32_t * const * bucket = *(const uint32_t * const **)(map + MAT_MAP_BUCKETS_OFF);
        if (!bucket)
            continue;
        int vsize = k_map_value_size[m];
        if (!vsize)
            return -1;
        const uint32_t * node = (const uint32_t *)*bucket;
        while (node) {
            const char * key = (const char *)node[1];
            int klen = key ? (int)strnlen(key, MAP_KEY_MAX) : 0;
            if (len + 2 + klen + vsize > MAP_SNAP_MAX)
                return -1;
            buf[len++] = (uint8_t)m;
            buf[len++] = (uint8_t)klen;
            memcpy(buf + len, key, (size_t)klen);
            len += klen;
            memcpy(buf + len, node + 2, (size_t)vsize);
            len += vsize;
            node = (const uint32_t *)node[0];
            while (!node) {
                bucket++;
                node = (const uint32_t *)*bucket;
            }
            if (node == (const uint32_t *)bucket)
                node = NULL;
        }
    }
    return len;
}

static int material_diff(const uint8_t * src, const pool_t * e) {
    if (memcmp(src, e->snap, MAT_SIZE) != 0)
        return R_BYTES;
    int populated = 0;
    for (int m = 0; m < MAT_MAP_COUNT; m++) {
        const uint8_t * map = src + MAT_MAP_FIRST + m * MAT_MAP_STRIDE;
        if (*(const uint32_t *)(map) != 0 || *(const uint32_t *)(map + MAT_MAP_SIZE_OFF) != 0)
            populated = 1;
    }
    if (populated) {
        if (!g_deep_on || !e->maps_ok)
            return R_MAPS;
        uint8_t buf[MAP_SNAP_MAX];
        int len = rcc_maps_serialise(src, buf);
        if (len < 0 || len != e->maps_len || memcmp(buf, e->maps, (size_t)len) != 0)
            return R_MAPS;
    }
    const uint32_t * tb = *(const uint32_t * const *)(src + MAT_TEXTURES);
    const uint32_t * te = *(const uint32_t * const *)(src + MAT_TEXTURES + 4);
    uint32_t ntex = (uint32_t)(te - tb) / 2;
    if (ntex != e->ntex || ntex > MAX_TEXTURES)
        return R_TEX;
    if (ntex && memcmp(tb, e->tex, ntex * 8) != 0)
        return R_TEX;
    const char * name = *(const char * const *)(src + MAT_SHADER_NAME);
    if (!name || strncmp(name, e->name, NAME_MAX - 1) != 0)
        return R_NAME;
    return R_SAME;
}

static void snapshot(pool_t * e, const uint8_t * src) {
    memcpy(e->snap, src, MAT_SIZE);
    const uint32_t * tb = *(const uint32_t * const *)(src + MAT_TEXTURES);
    const uint32_t * te = *(const uint32_t * const *)(src + MAT_TEXTURES + 4);
    uint32_t ntex = (uint32_t)(te - tb) / 2;
    e->ntex = ntex > MAX_TEXTURES ? MAX_TEXTURES + 1 : ntex;
    if (ntex && ntex <= MAX_TEXTURES) memcpy(e->tex, tb, ntex * 8);
    const char * name = *(const char * const *)(src + MAT_SHADER_NAME);
    if (name) {
        strncpy(e->name, name, NAME_MAX - 1);
        e->name[NAME_MAX - 1] = 0;
    } else {
        e->name[0] = 0;
    }
    int len = rcc_maps_serialise(src, e->maps);
    e->maps_ok  = len >= 0;
    e->maps_len = len >= 0 ? (uint16_t)len : 0;
}

static void pool_evict_stale(void) {
    if (g_flush_no % EVICT_AFTER)
        return;
    for (int i = 0; i < POOL_SIZE; i++) {
        pool_t * e = &g_pool[i];
        if (e->src && g_flush_no - e->last_flush > EVICT_AFTER)
            pool_destroy(e);
    }
}

static uint32_t rcc_force_change(void * self_) {
    uint8_t * self = self_;
    if (!g_gate_on)
        return SO_CONTINUE(uint32_t, g_force_hook, self);

    const uint8_t * sb = *(uint8_t **)(self + DSB_SPR_BEGIN);
    const uint8_t * se = *(uint8_t **)(self + DSB_SPR_END);
    if (sb == se)
        return 0;

    uint32_t total = (uint32_t)(se - sb) / SPRITE_DATA_SIZE;
    uint32_t n = *(uint32_t *)(self + DSB_SPRITES_IN_CMD);
    *(uint32_t *)(self + DSB_SPRITES_IN_CMD) = 0;
    c_changes++;

    inst_t * in = inst_for(self);
    const uint8_t * src = *(const uint8_t **)(self + DSB_LAST_MAT_PX);
    if (!in || !src || in->ncmds >= MAX_CMDS) {
        c_dropped++;
        return 0;
    }
    int fresh = 0;
    pool_t * e = pool_get(src, &fresh);
    if (!e) {
        c_dropped++;
        return 0;
    }
    int why = fresh ? R_FRESH : material_diff(src, e);
    if (why == R_SAME) {
        c_skipped++;
    } else {
        g_mat_assign(e->cmd, src);
        snapshot(e, src);
        c_assigned++;
        c_why[why]++;
    }
    e->last_flush = g_flush_no;
    cmd_t * c = &in->cmds[in->ncmds++];
    c->p = e;
    c->offset = (total - n) * 12;
    c->count  = n * 6;
    return 0;
}

static uint32_t rcc_flush(void * self_, void * rs) {
    uint8_t * self = self_;
    if (!g_gate_on)
        return SO_CONTINUE(uint32_t, g_flush_hook, self, rs);

    inst_t * in = inst_for(self);
    uint32_t idx = *(uint32_t *)(self + DSB_BUF_INDEX);
    void * batch = *(void **)(self + DSB_BATCH(idx));

    uint8_t * sb = *(uint8_t **)(self + DSB_SPR_BEGIN);
    uint8_t * se = *(uint8_t **)(self + DSB_SPR_END);
    if (sb != se) {
        g_batch_build(batch, self + DSB_SPR_BEGIN);
        g_destroy_sprites(sb, se);
        *(uint8_t **)(self + DSB_SPR_END) = sb;
    }

    if (in) {
        for (uint32_t i = 0; i < in->ncmds; i++) {
            cmd_t * c = &in->cmds[i];
            c->p->cmd[MAT_CACHE_VALID] = 0;
            g_batch_render(batch, rs, c->p->cmd, c->offset, c->count);
        }
        in->ncmds = 0;
    }
    *(uint32_t *)(self + DSB_BUF_INDEX) = (idx + 1 <= 1) ? idx + 1 : 0;
    g_flush_no++;
    c_flushes++;
    pool_evict_stale();
    return 0;
}

static uint32_t rcc_dtor(void * self_) {
    uint8_t * self = self_;
    for (int i = 0; i < MAX_INSTANCES; i++)
        if (g_inst[i].self == self) { g_inst[i].self = NULL; g_inst[i].ncmds = 0; }
    return SO_CONTINUE(uint32_t, g_dtor_hook, self);
}

void render_command_cache_install(void) {
    uintptr_t force  = so_symbol(&so_mod, SYM_FORCE_CHANGE);
    uintptr_t flush  = so_symbol(&so_mod, SYM_FLUSH);
    uintptr_t dtor   = so_symbol(&so_mod, SYM_DSB_DTOR);
    g_mat_ctor        = (mat_ctor_fn)so_symbol(&so_mod, SYM_MAT_CTOR);
    g_mat_dtor        = (mat_dtor_fn)so_symbol(&so_mod, SYM_MAT_DTOR);
    g_mat_assign      = (mat_assign_fn)so_symbol(&so_mod, SYM_MAT_ASSIGN);
    g_batch_build     = (batch_build_fn)so_symbol(&so_mod, SYM_BATCH_BUILD);
    g_batch_render    = (batch_render_fn)so_symbol(&so_mod, SYM_BATCH_RENDER);
    g_destroy_sprites = (destroy_sprites_fn)so_symbol(&so_mod, SYM_DESTROY_SPRITES);

    if (!force || !flush || !dtor || !g_mat_ctor || !g_mat_dtor || !g_mat_assign ||
        !g_batch_build || !g_batch_render || !g_destroy_sprites) {
        l_error("render_command_cache: a symbol is missing (force=%p flush=%p dtor=%p ctor=%p "
                "mdtor=%p assign=%p build=%p render=%p destroy=%p) - batch runs stock",
                (void *)force, (void *)flush, (void *)dtor, (void *)g_mat_ctor,
                (void *)g_mat_dtor, (void *)g_mat_assign, (void *)g_batch_build,
                (void *)g_batch_render, (void *)g_destroy_sprites);
        return;
    }
    g_force_hook = hook_addr(force, (uintptr_t)&rcc_force_change);
    g_flush_hook = hook_addr(flush, (uintptr_t)&rcc_flush);
    g_dtor_hook  = hook_addr(dtor,  (uintptr_t)&rcc_dtor);
    l_perf("[render-cmd] CDynamicSpriteBatch::ForceCommandChange and BuildAndFlushBatch "
           "replaced - a RenderCommand's CMaterial lives across frames and is re-assigned "
           "only when its source changed (pool of %d copies keyed by source, %d batches)", POOL_SIZE, MAX_INSTANCES);
}
