#include "utils/ai_board_copy.h"

#include <stdint.h>
#include <psp2/kernel/processmgr.h>
#include <so_util/so_util.h>

#include "utils/logger.h"
#include "utils/update_profile.h"

extern so_module so_mod;

#define GBCOPY_SYM   "_ZN9GameBoard4copyEPS_"
#define ENTCOPY_SYM  "_ZN6Entity4copyEv"
#define DESTROYE_SYM "_ZN9GameBoard13destroyEntityEP6Entity"
#define DESTROYF_SYM "_ZN9GameBoard16destroyFormationEP5Units"
#define SETPROP_SYM  "_ZN6Entity11setPropertyEi"
#define FXCLEAR_SYM  "_ZN12StatusEffect5clearEP6Entityi"
#define METERDEL_SYM "_ZN11EntityMeter7destroyEv"
#define CSICON_SYM   "_ZN6Entity20deinitCheatSheetIconEv"
#define CSNUM_SYM    "_ZN6Entity22deinitCheatSheetNumberEv"
#define DEINITSPR_SYM "_ZN6Entity13deinitSpritesEv"
#define DEINITSHD_SYM "_ZN6Entity13deinitShadowsEv"
#define RMEFFECT_SYM "_ZN6Entity14removeEffect3dEv"
#define FUSIONHALO_SYM "_ZN5Units17destroyFusionHaloEv"

#define GB_ENTITIES   0x004
#define GB_NUM_ENT    0x304
#define GB_FORMATIONS 0x308
#define GB_NUM_FORM   0x488
#define GB_COLS       0x4d4
#define GB_ROWS       0x4d8
#define GB_GRID       0x4dc
#define GB_QUEUED     0x7dc

#define AIBC_MAX_ENT  192
#define AIBC_MAX_COL  16
#define AIBC_MAX_ROW  12
#define GRID_COL_STRIDE 0x30

#define E_VPTR     0x00
#define E_ALIVE    0x04
#define E_FLAG5    0x05
#define E_TYPE     0x06
#define E_ID       0x07
#define E_GETID    0x08
#define E_SIDE     0x0a
#define E_PIDX     0x0b
#define E_WIDTH    0x28
#define E_HEIGHT   0x29
#define E_PROPS    0x38
#define E_POWER    0x48
#define E_MAXPOWER 0x4a
#define E_POWER3   0x4c
#define E_PLAYER   0x50
#define E_ONE58    0x58
#define E_METER    0x8c
#define E_CS_FLAG  0xc0
#define E_UNITSIDE 0xd6
#define E_FUSION   0xe4

#define PROP_BOARD_TEARDOWN 0x02000000

#define VT_GETID      3
#define VT_GETWIDTH   4
#define VT_GETHEIGHT  5
#define VT_GETMAXPOWER 6

#define E_INIT_1C_BITS 0x41400000u

enum { T_UNITS = 0, T_WALL, T_PROJECTILE, T_BLAST, T_TARGET, T_BOSS, T_BOSSPART, T_COUNT };

static const uint8_t k_reusable[T_COUNT] = { 1, 1, 0, 1, 0, 0, 1 };

static const char *const k_type_init[T_COUNT] = {
	"_ZN5Units4initEPS_",  "_ZN4Wall4initEPS_",  "_ZN10Projectile4initEPS_",
	"_ZN5Blast4initEPS_",  "_ZN6Target4initEPS_", "_ZN4Boss4initEPS_",
	"_ZN8BossPart4initEPS_",
};
static const char *const k_type_copy[T_COUNT] = {
	"_ZN5Units4copyEPS_S0_",  "_ZN4Wall4copyEPS_S0_",  "_ZN10Projectile4copyEPS_S0_",
	"_ZN5Blast4copyEPS_S0_",  "_ZN6Target4copyEPS_S0_", "_ZN4Boss4copyEPS_S0_",
	"_ZN8BossPart4copyEPS_S0_",
};

typedef void  (*void1_fn)(void *self);
typedef void  (*void2_fn)(void *a, void *b);
typedef void  (*setprop_fn)(void *self, int mask);
typedef void  (*fxclear_fn)(void *entity, int kind);
typedef void *(*entcopy_fn)(void *self);
typedef int   (*vtgetter_fn)(void *self);

static so_hook    g_hook;
static entcopy_fn g_entity_copy;
static void2_fn   g_destroy_entity;
static void2_fn   g_destroy_formation;
static setprop_fn g_set_property;
static fxclear_fn g_fx_clear;
static void1_fn   g_meter_destroy;
static void1_fn   g_deinit_cs_icon;
static void1_fn   g_deinit_cs_number;
static void1_fn   g_deinit_sprites;
static void1_fn   g_deinit_shadows;
static void1_fn   g_remove_effect3d;
static void1_fn   g_destroy_fusion_halo;
static void1_fn   g_type_init[T_COUNT];
static void2_fn   g_type_copy[T_COUNT];

static void    *g_old[AIBC_MAX_ENT];
static void    *g_reuse[AIBC_MAX_ENT];
static uint8_t  g_claimed[AIBC_MAX_ENT];

#define ARM_ON() (1)

static inline uint32_t rd32(const void *p, uint32_t off)
{
	return *(const volatile uint32_t *)((const volatile uint8_t *)p + off);
}
static inline void wr32(void *p, uint32_t off, uint32_t v)
{
	*(volatile uint32_t *)((volatile uint8_t *)p + off) = v;
}
static inline uint16_t rd16(const void *p, uint32_t off)
{
	return *(const volatile uint16_t *)((const volatile uint8_t *)p + off);
}
static inline void wr16(void *p, uint32_t off, uint16_t v)
{
	*(volatile uint16_t *)((volatile uint8_t *)p + off) = v;
}
static inline uint8_t rd8(const void *p, uint32_t off)
{
	return *(const volatile uint8_t *)((const volatile uint8_t *)p + off);
}
static inline void wr8(void *p, uint32_t off, uint8_t v)
{
	*(volatile uint8_t *)((volatile uint8_t *)p + off) = v;
}
static inline int rds8(const void *p, uint32_t off)
{
	return (int)(int8_t)rd8(p, off);
}

static inline void **grid_cell(void *board, int col, int row)
{
	return (void **)((uint8_t *)board + GB_GRID + (uint32_t)col * GRID_COL_STRIDE
	                 + (uint32_t)row * 4u);
}

static void entity_init_inplace(void *e, void *player, int type, int id, int unit_side)
{
	void **vt = *(void ***)e;

	if (type == T_UNITS)
		wr8(e, E_UNITSIDE, (uint8_t)unit_side);
	wr8(e, E_ID, (uint8_t)id);
	wr32(e, E_PLAYER, (uint32_t)player);
	wr32(e, E_PLAYER + 4, (uint32_t)player);
	wr8(e, E_TYPE, (uint8_t)type);
	wr8(e, E_SIDE, (uint8_t)rd32(player, 0xe18));
	wr8(e, E_PIDX, (uint8_t)rd32((void *)rd32(player, 0xde4), 0x4cc));

	wr16(e, E_GETID, (uint16_t)((vtgetter_fn)vt[VT_GETID])(e));
	wr8(e, E_FLAG5, 0);
	wr32(e, 0x1c, E_INIT_1C_BITS);
	wr8(e, E_ALIVE, 1);
	wr32(e, 0x0c, 0);
	wr32(e, 0x10, 0);
	wr32(e, 0x20, 0);
	wr32(e, 0x24, 0);
	wr32(e, 0x14, 0);
	wr32(e, 0x18, 0);

	wr8(e, E_WIDTH,  (uint8_t)((vtgetter_fn)vt[VT_GETWIDTH])(e));
	wr8(e, E_HEIGHT, (uint8_t)((vtgetter_fn)vt[VT_GETHEIGHT])(e));

	wr32(e, 0x2c, 0);
	wr32(e, 0x30, 0);
	wr32(e, 0x40, 0);
	wr32(e, 0x3c, 0);
	wr32(e, 0x44, 0);

	wr16(e, E_POWER,    (uint16_t)((vtgetter_fn)vt[VT_GETMAXPOWER])(e));
	wr16(e, E_MAXPOWER, (uint16_t)((vtgetter_fn)vt[VT_GETMAXPOWER])(e));
	wr16(e, E_POWER3, 0);

	wr8(e, 0xa7, 0xff);
	wr8(e, 0xa5, 0xff);
	wr8(e, 0xa6, 0xff);

	g_deinit_sprites(e);
	g_deinit_shadows(e);

	wr32(e, E_ONE58, 1);
	wr32(e, 0x90, 0);
	wr32(e, 0x94, 0);
	wr32(e, 0x98, 0);
	wr8(e, 0xa9, 0);
	wr32(e, 0x9c, 0);
	wr32(e, 0xa0, 0);

	g_remove_effect3d(e);

	wr32(e, 0xb0, 0);
	wr32(e, 0xac, 0);
	wr8(e, 0xb4, 0);

	g_type_init[type](e);

	wr8(e, 0xb5, 0);
	wr8(e, 0xb6, 0);
}

static void entity_copy_fields(const void *s, void *d)
{
	wr16(d, 0x48, rd16(s, 0x48));
	wr16(d, 0x4a, rd16(s, 0x4a));
	wr16(d, 0x4c, rd16(s, 0x4c));
	wr32(d, 0x0c, rd32(s, 0x0c));
	wr32(d, 0x10, rd32(s, 0x10));
	wr32(d, 0x20, rd32(s, 0x20));
	wr32(d, 0x24, rd32(s, 0x24));
	wr32(d, 0x14, rd32(s, 0x14));
	wr32(d, 0x18, rd32(s, 0x18));
	wr32(d, 0x1c, rd32(s, 0x1c));
	wr8(d, 0x2a, rd8(s, 0x2a));
	wr8(d, 0x2b, rd8(s, 0x2b));
	wr32(d, 0x2c, rd32(s, 0x2c));
	wr32(d, 0x30, rd32(s, 0x30));
	wr32(d, 0x34, rd32(s, 0x34));
	wr32(d, 0x40, rd32(s, 0x40));
	wr32(d, 0x44, rd32(s, 0x44));
	wr32(d, 0x38, rd32(s, 0x38));
	wr8(d, 0xa4, rd8(s, 0xa4));
	wr8(d, 0xa7, rd8(s, 0xa7));
}

static void entity_recycle_teardown(void *d, int type)
{
	g_fx_clear(d, -1);
	g_meter_destroy((void *)rd32(d, E_METER));
	g_deinit_cs_icon(d);
	g_deinit_cs_number(d);
	wr8(d, E_CS_FLAG, 0);
	if (type == T_UNITS) {
		g_destroy_fusion_halo(d);
		wr32(d, E_FUSION, 0);
	}
}

static void stamp_footprint(void *dst, const void *src, void *se, void *de, int cols, int rows)
{
	for (int row = 0; row < rows; row++) {
		for (int col = 0; col < cols; col++) {
			if (*grid_cell((void *)src, col, row) != se)
				continue;
			int w = rds8(se, E_WIDTH);
			int h = rds8(se, E_HEIGHT);
			for (int rr = 0; rr < h; rr++)
				for (int cc = 0; cc < w; cc++)
					*grid_cell(dst, col + cc, row + rr) = de;
			return;
		}
	}
}

static void gameboard_copy_c(void *dst, void *src)
{
#define AIBC_DONE()  ((void)0)
	if (!ARM_ON()) {
		(void)SO_CONTINUE(int, g_hook, dst, src);
		return;
	}

	const int cols   = (int)rd32(dst, GB_COLS);
	const int rows   = (int)rd32(dst, GB_ROWS);
	const int old_n  = (int)rd32(dst, GB_NUM_ENT);
	const int src_n  = (int)rd32(src, GB_NUM_ENT);

	if (old_n < 0 || old_n > AIBC_MAX_ENT || src_n < 0 || src_n > AIBC_MAX_ENT ||
	    cols < 0 || cols > AIBC_MAX_COL || rows < 0 || rows > AIBC_MAX_ROW) {
		(void)SO_CONTINUE(int, g_hook, dst, src);
		return;
	}

	void **dst_ent = (void **)((uint8_t *)dst + GB_ENTITIES);
	void **src_ent = (void **)((uint8_t *)src + GB_ENTITIES);

	for (int i = 0; i < old_n; i++) {
		g_old[i] = dst_ent[i];
		g_claimed[i] = 0;
	}

	for (int i = 0; i < src_n; i++) {
		g_reuse[i] = 0;
		int type = rds8(src_ent[i], E_TYPE);
		if (type < 0 || type >= T_COUNT)
			continue;
		if (!k_reusable[type]) {
			continue;
		}
		if (i < old_n && !g_claimed[i] && rds8(g_old[i], E_TYPE) == type) {
			g_claimed[i] = 1;
			g_reuse[i] = g_old[i];
			continue;
		}
		for (int j = 0; j < old_n; j++) {
			if (g_claimed[j] || rds8(g_old[j], E_TYPE) != type)
				continue;
			g_claimed[j] = 1;
			g_reuse[i] = g_old[j];
			break;
		}
	}

	for (int j = 0; j < old_n; j++) {
		if (g_claimed[j])
			continue;
		g_set_property(g_old[j], PROP_BOARD_TEARDOWN);
		g_destroy_entity(dst, g_old[j]);
	}

	void **queued = (void **)((uint8_t *)dst + GB_QUEUED);
	for (int col = 0; col < cols; col++) {
		for (int j = 0; j < old_n; j++) {
			if (queued[col] == g_old[j]) {
				queued[col] = 0;
				break;
			}
		}
	}

	for (int row = 0; row < rows; row++)
		for (int col = 0; col < cols; col++)
			*grid_cell(dst, col, row) = 0;

	wr32(dst, GB_NUM_ENT, (uint32_t)src_n);
	for (int i = 0; i < src_n; i++) {
		void *se = src_ent[i];
		void *de = g_reuse[i];
		if (de) {
			int type = rds8(se, E_TYPE);
			entity_recycle_teardown(de, type);
			entity_init_inplace(de, (void *)rd32(se, E_PLAYER), type,
			                    rds8(se, E_ID),
			                    type == T_UNITS ? rds8(se, E_UNITSIDE) : -1);
			entity_copy_fields(se, de);
			g_type_copy[type](se, de);
		} else {
			de = g_entity_copy(se);
		}
		dst_ent[i] = de;
		stamp_footprint(dst, src, se, de, cols, rows);
	}

	while ((int)rd32(dst, GB_NUM_FORM) > 0)
		g_destroy_formation(dst, *(void **)((uint8_t *)dst + GB_FORMATIONS));

	const int form_n = (int)rd32(src, GB_NUM_FORM);
	wr32(dst, GB_NUM_FORM, (uint32_t)form_n);
	void **dst_form = (void **)((uint8_t *)dst + GB_FORMATIONS);
	void **src_form = (void **)((uint8_t *)src + GB_FORMATIONS);
	for (int j = 0; j < form_n; j++) {
		int n = (int)rd32(dst, GB_NUM_ENT);
		for (int k = 0; k < n; k++) {
			if (src_form[j] == src_ent[k]) {
				dst_form[j] = dst_ent[k];
				break;
			}
		}
	}

#undef AIBC_DONE
}

void ai_board_copy_install(void)
{
	uintptr_t copy = so_symbol(&so_mod, GBCOPY_SYM);

	g_entity_copy         = (entcopy_fn)so_symbol(&so_mod, ENTCOPY_SYM);
	g_destroy_entity      = (void2_fn)so_symbol(&so_mod, DESTROYE_SYM);
	g_destroy_formation   = (void2_fn)so_symbol(&so_mod, DESTROYF_SYM);
	g_set_property        = (setprop_fn)so_symbol(&so_mod, SETPROP_SYM);
	g_fx_clear            = (fxclear_fn)so_symbol(&so_mod, FXCLEAR_SYM);
	g_meter_destroy       = (void1_fn)so_symbol(&so_mod, METERDEL_SYM);
	g_deinit_cs_icon      = (void1_fn)so_symbol(&so_mod, CSICON_SYM);
	g_deinit_cs_number    = (void1_fn)so_symbol(&so_mod, CSNUM_SYM);
	g_deinit_sprites      = (void1_fn)so_symbol(&so_mod, DEINITSPR_SYM);
	g_deinit_shadows      = (void1_fn)so_symbol(&so_mod, DEINITSHD_SYM);
	g_remove_effect3d     = (void1_fn)so_symbol(&so_mod, RMEFFECT_SYM);
	g_destroy_fusion_halo = (void1_fn)so_symbol(&so_mod, FUSIONHALO_SYM);

	int missing = !copy || !g_entity_copy || !g_destroy_entity ||
	              !g_destroy_formation || !g_set_property || !g_fx_clear ||
	              !g_meter_destroy || !g_deinit_cs_icon || !g_deinit_cs_number ||
	              !g_deinit_sprites || !g_deinit_shadows || !g_remove_effect3d ||
	              !g_destroy_fusion_halo;

	for (int t = 0; t < T_COUNT; t++) {
		g_type_init[t] = (void1_fn)so_symbol(&so_mod, k_type_init[t]);
		g_type_copy[t] = (void2_fn)so_symbol(&so_mod, k_type_copy[t]);
		if (k_reusable[t] && (!g_type_init[t] || !g_type_copy[t]))
			missing = 1;
	}

	if (missing) {
		l_error("aiboard-c: a symbol is missing (copy=%p entity_copy=%p "
		        "destroy_entity=%p fx_clear=%p) - NOT installed, "
		        "GameBoard::copy runs stock",
		        (void *)copy, (void *)g_entity_copy, (void *)g_destroy_entity,
		        (void *)g_fx_clear);
		return;
	}

	g_hook = hook_addr(copy, (uintptr_t)&gameboard_copy_c);
}
