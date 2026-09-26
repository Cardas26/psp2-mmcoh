#include "utils/softfloat_vfp.h"

#include <stdint.h>
#include <string.h>
#include <so_util/so_util.h>

#include "utils/logger.h"

extern so_module so_mod;

#define VENEER_W0 0x46c04778u
#define VENEER_W1 0xe59fc000u
#define VENEER_W2 0xe08cf00fu
#define VENEER_SZ 0x10u

typedef struct {
    const char * sym;
    uint16_t     in[2];
    uint16_t     op[2];
    uint16_t     out[2];
} sf_op;

#define ARG_FF   {0xec41, 0x0a10}
#define ARG_F    {0xee00, 0x0a10}
#define ARG_D    {0xec41, 0x0b10}
#define RET_F    {0xee10, 0x0a10}
#define RET_D    {0xec51, 0x0b10}

static const sf_op k_ops[] = {
    { "__aeabi_fmul",  ARG_FF, {0xee20, 0x0a20}, RET_F },
    { "__aeabi_fadd",  ARG_FF, {0xee30, 0x0a20}, RET_F },
    { "__aeabi_fsub",  ARG_FF, {0xee30, 0x0a60}, RET_F },
    { "__aeabi_frsub", ARG_FF, {0xee30, 0x0ac0}, RET_F },
    { "__aeabi_fdiv",  ARG_FF, {0xee80, 0x0a20}, RET_F },
    { "__aeabi_i2f",   ARG_F,  {0xeeb8, 0x0ac0}, RET_F },
    { "__aeabi_ui2f",  ARG_F,  {0xeeb8, 0x0a40}, RET_F },
    { "__aeabi_f2iz",  ARG_F,  {0xeebd, 0x0ac0}, RET_F },
    { "__aeabi_f2uiz", ARG_F,  {0xeebc, 0x0ac0}, RET_F },
    { "__aeabi_f2d",   ARG_F,  {0xeeb7, 0x0ac0}, RET_D },
    { "__aeabi_i2d",   ARG_F,  {0xeeb8, 0x0bc0}, RET_D },
    { "__aeabi_ui2d",  ARG_F,  {0xeeb8, 0x0b40}, RET_D },
    { "__aeabi_d2f",   ARG_D,  {0xeeb7, 0x0bc0}, RET_F },
    { "__aeabi_d2iz",  ARG_D,  {0xeebd, 0x0bc0}, RET_F },
    { "__aeabi_d2uiz", ARG_D,  {0xeebc, 0x0bc0}, RET_F },
};

#define N_OPS ((int)(sizeof(k_ops) / sizeof(k_ops[0])))

static inline float as_f32(uint32_t v) {
    float f;
    __builtin_memcpy(&f, &v, sizeof f);
    return f;
}

static inline double as_f64(uint64_t v) {
    double d;
    __builtin_memcpy(&d, &v, sizeof d);
    return d;
}

static inline uint64_t as_u64(double d) {
    uint64_t v;
    __builtin_memcpy(&v, &d, sizeof v);
    return v;
}

static int sf_fcmpeq(uint32_t a, uint32_t b) { return as_f32(a) == as_f32(b); }
static int sf_fcmplt(uint32_t a, uint32_t b) { return as_f32(a) <  as_f32(b); }
static int sf_fcmple(uint32_t a, uint32_t b) { return as_f32(a) <= as_f32(b); }
static int sf_fcmpge(uint32_t a, uint32_t b) { return as_f32(a) >= as_f32(b); }
static int sf_fcmpgt(uint32_t a, uint32_t b) { return as_f32(a) >  as_f32(b); }
static int sf_fcmpun(uint32_t a, uint32_t b) {
    return (a & 0x7fffffffu) > 0x7f800000u || (b & 0x7fffffffu) > 0x7f800000u;
}

static uint64_t sf_dadd(uint64_t a, uint64_t b) { return as_u64(as_f64(a) + as_f64(b)); }
static uint64_t sf_dsub(uint64_t a, uint64_t b) { return as_u64(as_f64(a) - as_f64(b)); }
static uint64_t sf_dmul(uint64_t a, uint64_t b) { return as_u64(as_f64(a) * as_f64(b)); }
static uint64_t sf_ddiv(uint64_t a, uint64_t b) { return as_u64(as_f64(a) / as_f64(b)); }

static int sf_dcmpeq(uint64_t a, uint64_t b) { return as_f64(a) == as_f64(b); }
static int sf_dcmplt(uint64_t a, uint64_t b) { return as_f64(a) <  as_f64(b); }
static int sf_dcmple(uint64_t a, uint64_t b) { return as_f64(a) <= as_f64(b); }
static int sf_dcmpge(uint64_t a, uint64_t b) { return as_f64(a) >= as_f64(b); }
static int sf_dcmpgt(uint64_t a, uint64_t b) { return as_f64(a) >  as_f64(b); }

typedef struct {
    const char * sym;
    uintptr_t    stub;
} sf_thunk;

static const sf_thunk k_thunks[] = {
    { "__aeabi_fcmpeq", (uintptr_t)&sf_fcmpeq },
    { "__aeabi_fcmplt", (uintptr_t)&sf_fcmplt },
    { "__aeabi_fcmple", (uintptr_t)&sf_fcmple },
    { "__aeabi_fcmpge", (uintptr_t)&sf_fcmpge },
    { "__aeabi_fcmpgt", (uintptr_t)&sf_fcmpgt },
    { "__aeabi_fcmpun", (uintptr_t)&sf_fcmpun },
    { "__aeabi_dadd",   (uintptr_t)&sf_dadd   },
    { "__aeabi_dsub",   (uintptr_t)&sf_dsub   },
    { "__aeabi_dmul",   (uintptr_t)&sf_dmul   },
    { "__aeabi_ddiv",   (uintptr_t)&sf_ddiv   },
    { "__aeabi_dcmpeq", (uintptr_t)&sf_dcmpeq },
    { "__aeabi_dcmplt", (uintptr_t)&sf_dcmplt },
    { "__aeabi_dcmple", (uintptr_t)&sf_dcmple },
    { "__aeabi_dcmpge", (uintptr_t)&sf_dcmpge },
    { "__aeabi_dcmpgt", (uintptr_t)&sf_dcmpgt },
};

#define N_THUNKS ((int)(sizeof(k_thunks) / sizeof(k_thunks[0])))

static void write_thunk(uintptr_t veneer, uintptr_t stub) {
    uint16_t * p = (uint16_t *)veneer;
    p[0] = 0xf8df;
    p[1] = 0xf000;
    *(uint32_t *)(veneer + 4) = (uint32_t)stub;
    p[4] = 0xbf00;
    p[5] = 0xbf00;
    p[6] = 0xbf00;
    p[7] = 0xbf00;
}

static void write_stub(uintptr_t veneer, const sf_op * op) {
    uint16_t * p = (uint16_t *)veneer;
    p[0] = op->in[0];  p[1] = op->in[1];
    p[2] = op->op[0];  p[3] = op->op[1];
    p[4] = op->out[0]; p[5] = op->out[1];
    p[6] = 0x4770;
    p[7] = 0xbf00;
}

extern so_module so_mod_fmodex;

#define ARM_WORD(hw)  (((uint32_t)(hw)[0] << 16) | (uint32_t)(hw)[1])
#define ARM_BX_LR     0xe12fff1eu
#define ARM_LDR_PC    0xe51ff004u
#define ARM_B(from, to) \
    (0xea000000u | ((((uint32_t)(to) - ((uint32_t)(from) + 8)) >> 2) & 0xffffffu))

typedef struct {
    const char * sym;
    uint32_t     stock;
} fmod_body;

static const fmod_body k_fmod_ops[] = {
    { "__aeabi_fmul",  0xe3a0c0ffu },
    { "__aeabi_fadd",  0xe1b02080u },
    { "__aeabi_fdiv",  0xe3a0c0ffu },
    { "__aeabi_f2iz",  0xe1a02080u },
    { "__aeabi_f2uiz", 0xe1b02080u },
};

static const char * const k_fmod_cmps[] = {
    "__aeabi_fcmpeq", "__aeabi_fcmplt", "__aeabi_fcmple",
    "__aeabi_fcmpge", "__aeabi_fcmpgt",
};
#define FMOD_CMP_STOCK 0xe52de008u

static const sf_op * find_op(const char * sym) {
    for (int i = 0; i < N_OPS; i++)
        if (strcmp(k_ops[i].sym, sym) == 0)
            return &k_ops[i];
    return NULL;
}

static uintptr_t fmod_body_at(const char * sym, uint32_t stock) {
    uintptr_t a = so_symbol(&so_mod_fmodex, sym);
    if (a == 0 || (a & 1u) || *(const uint32_t *)a != stock) {
        l_warn("[softfloat-vfp] libfmodex.so %s: %s, left as software\n", sym,
               a == 0 ? "not exported" : "not the expected ARM entry");
        return 0;
    }
    return a;
}

static void write_arm_op(uintptr_t at, const sf_op * op) {
    uint32_t * w = (uint32_t *)at;
    w[0] = ARM_WORD(op->in);
    w[1] = ARM_WORD(op->op);
    w[2] = ARM_WORD(op->out);
    w[3] = ARM_BX_LR;
}

static void softfloat_vfp_fmod_install(void) {
    int done = 0, total = 0;

    for (int i = 0; i < (int)(sizeof(k_fmod_ops) / sizeof(k_fmod_ops[0])); i++) {
        total++;
        uintptr_t a = fmod_body_at(k_fmod_ops[i].sym, k_fmod_ops[i].stock);
        if (a == 0)
            continue;
        write_arm_op(a, find_op(k_fmod_ops[i].sym));
        done++;
    }

    total += 2;
    uintptr_t i2f = fmod_body_at("__aeabi_i2f", 0xe2103102u);
    uintptr_t ui2f = fmod_body_at("__aeabi_ui2f", 0xe3a03000u);
    if (i2f != 0 && ui2f != 0 && ui2f + 8 == i2f &&
        *(const uint32_t *)(ui2f + 4) == ARM_B(ui2f + 4, i2f + 8)) {
        write_arm_op(i2f, find_op("__aeabi_i2f"));
        write_arm_op(i2f + 16, find_op("__aeabi_ui2f"));
        *(uint32_t *)ui2f = ARM_B(ui2f, i2f + 16);
        done += 2;
    } else if (i2f != 0 && ui2f != 0) {
        l_warn("[softfloat-vfp] libfmodex.so i2f/ui2f are not the expected "
               "pair, left as software\n");
    }

    for (int i = 0; i < (int)(sizeof(k_fmod_cmps) / sizeof(k_fmod_cmps[0])); i++) {
        total++;
        uintptr_t a = fmod_body_at(k_fmod_cmps[i], FMOD_CMP_STOCK);
        if (a == 0)
            continue;
        uintptr_t stub = 0;
        for (int j = 0; j < N_THUNKS; j++)
            if (strcmp(k_thunks[j].sym, k_fmod_cmps[i]) == 0)
                stub = k_thunks[j].stub;
        uint32_t * w = (uint32_t *)a;
        w[0] = ARM_LDR_PC;
        w[1] = (uint32_t)stub;
        done++;
    }

    l_info("[softfloat-vfp] libfmodex.so: %d of %d soft-float helper bodies "
           "rewritten as VFP\n", done, total);
}

void softfloat_vfp_install(void) {
    uintptr_t op_target[N_OPS];
    int op_hits[N_OPS];
    int found = 0, total = 0;

    for (int i = 0; i < N_OPS; i++) {
        op_target[i] = so_symbol(&so_mod, k_ops[i].sym) & ~1u;
        op_hits[i] = 0;
        if (op_target[i] == 0)
            l_warn("[softfloat-vfp] %s not exported, left as software\n",
                   k_ops[i].sym);
    }

    uintptr_t th_target[N_THUNKS];
    int th_hits[N_THUNKS];

    for (int i = 0; i < N_THUNKS; i++) {
        th_target[i] = so_symbol(&so_mod, k_thunks[i].sym) & ~1u;
        th_hits[i] = 0;
        if (th_target[i] == 0)
            l_warn("[softfloat-vfp] %s not exported, left as software\n",
                   k_thunks[i].sym);
    }

    uintptr_t base = so_mod.text_base;
    uintptr_t end = base + so_mod.text_size - VENEER_SZ;
    for (uintptr_t v = base; v <= end; v += 4) {
        const uint32_t * w = (const uint32_t *)v;
        if (w[0] != VENEER_W0 || w[1] != VENEER_W1 || w[2] != VENEER_W2)
            continue;
        total++;
        uintptr_t dst = v + VENEER_SZ + (uintptr_t)(int32_t)w[3];

        int done = 0;
        for (int i = 0; i < N_OPS && !done; i++) {
            if (op_target[i] == 0 || dst != op_target[i])
                continue;
            write_stub(v, &k_ops[i]);
            op_hits[i]++;
            found++;
            done = 1;
        }
        for (int i = 0; i < N_THUNKS && !done; i++) {
            if (th_target[i] == 0 || dst != th_target[i])
                continue;
            write_thunk(v, k_thunks[i].stub);
            th_hits[i]++;
            found++;
            done = 1;
        }
    }

    l_info("[softfloat-vfp] %d of %d interworking veneers rewritten as VFP "
           "(%d inline, %d thunked)\n", found, total, N_OPS, N_THUNKS);
    for (int i = 0; i < N_OPS; i++) {
        if (op_target[i] != 0 && op_hits[i] == 0)
            l_warn("[softfloat-vfp] no veneer reaches %s (0x%x); its call "
                   "sites, if any, stay in software\n",
                   k_ops[i].sym, (unsigned)op_target[i]);
    }
    for (int i = 0; i < N_THUNKS; i++) {
        if (th_target[i] != 0 && th_hits[i] == 0)
            l_warn("[softfloat-vfp] no veneer reaches %s (0x%x); its call "
                   "sites, if any, stay in software\n",
                   k_thunks[i].sym, (unsigned)th_target[i]);
    }

    softfloat_vfp_fmod_install();
}
