#include "utils/sprite_setter_c.h"

#include <stdint.h>
#include <string.h>
#include <so_util/so_util.h>

#include "utils/logger.h"
#include "utils/sprite_state.h"

extern so_module so_mod;

#define SETPOS_SYM    "_ZN9NewSprite11setPositionEfff"
#define SETGAIN_SYM   "_ZN9NewSprite7setGainERKN5moFlo4Core8CVector4E"
#define SETALPHA_SYM  "_ZN9NewSprite8setAlphaEf"
#define SETRGB_SYM    "_ZN9NewSprite6setRGBEfff"
#define SETVIS_SYM    "_ZN9NewSprite10setVisibleEb"
#define SETPARAM_SYM  "_ZN15IShaderInstance18setShaderParameterEPKcRKN5moFlo4Core8CVector4E"
#define SETCOLOUR_SYM "_ZN5moFlo9Rendering16CSpriteComponent9SetColourEffff"
#define MESH_VIS_SYM   "_ZN4Mesh10setVisibleEb"
#define INODE_VIS_SYM  "_ZN5INode10setVisibleEb"
#define BRANCH_VIS_SYM "_ZN11SceneBranch10setVisibleEb"

#define NS_CTRL   0xB0
#define NS_TYPE   0xC0
#define NS_GAIN   0xCC
#define NS_COMPS  0x110
#define NS_LOADED 0xB9

#define OBJ_PNODE 0x4
#define PN_ENTITY 0x4
#define ENT_VIS   0x124

#define ENT_OBJ   0x8
#define OBJ_SPTR  0x24
#define OBJ_SHADER 0x8

typedef void (*setparam_fn)(void *inst, const char *name, const uint32_t *v4);
typedef void (*setcolour_fn)(void *comp, uint32_t r, uint32_t g, uint32_t b, uint32_t a);
typedef int  (*setvis_fn)(void *self, uint32_t vis);

static setparam_fn   g_setparam;
static setcolour_fn  g_setcolour;
static so_hook        g_pos_hook;
static int            g_installed;
static uintptr_t     g_mesh_vis, g_inode_vis, g_branch_vis;

static inline float f_of(uint32_t b) { float f; memcpy(&f, &b, sizeof f); return f; }
static inline uint32_t b_of(float f) { uint32_t b; memcpy(&b, &f, sizeof b); return b; }

#define SPRITE_WR32(s, off, v) (*(volatile uint32_t *)((volatile uint8_t *)(s) + (off)) = (v))

static uint32_t ysorted_z_bits(const uint8_t *s, float y, float prio) {
	int addp = 1;
	if (SPRITE_RD8(s, F_FLIP_V)) {
		int32_t t = (int32_t)SPRITE_RD32(s, NS_TYPE);
		if (t > 374) {
			if (t < 436)       addp = 0;
			else if (t <= 438) addp = 1;
			else               addp = ((uint32_t)(t - 546) <= 2u);
		}
		else if (t >= 372) addp = 1;
		else if (t < 229)  addp = 0;
		else if (t <= 231) addp = 1;
		else               addp = ((uint32_t)(t - 338) <= 2u);
	}
	static const volatile float k_div = 4000.0f;
	float d = y * 0.95f / k_div;
	return b_of(addp ? prio + d : prio - d);
}

static void setpos_predict(const uint8_t *s, uint32_t xb, uint32_t yb, uint32_t zb,
                           uint32_t *out_x, uint32_t *out_y, uint32_t *out_z) {
	*out_x = xb;
	*out_y = yb ^ SPRITE_FLOAT_NEG_BITS;
	if (SPRITE_RD8(s, F_YSORT)) {
		float rh = 0.0f;
		const uint8_t *ctrl = (const uint8_t *)SPRITE_RD32(s, NS_CTRL);
		if (ctrl) {
			const uint8_t *a = (const uint8_t *)(*(const uint32_t *)(ctrl + 0x18));
			if (a) rh = f_of(*(const uint32_t *)(a + 8));
		}
		*out_z = ysorted_z_bits(s, f_of(yb) + rh * 0.5f,
		                        (float)(int32_t)SPRITE_RD32(s, F_PRIORITY));
	} else {
		*out_z = zb;
	}
}

static void setpos_c(void *self, uint32_t xb, uint32_t yb, uint32_t zb) {
	uint8_t *s = self;
	uint32_t x, y, z;
	setpos_predict(s, xb, yb, zb, &x, &y, &z);
	SPRITE_WR8(s, F_DIRTY, 1);
	SPRITE_WR32(s, F_POS_X, x);
	SPRITE_WR32(s, F_POS_Z, z);
	SPRITE_WR32(s, F_POS_Y, y);
}

static void setgain_c(void *self, const uint32_t *v) {
	uint8_t *s = self;
	uint8_t *node = (uint8_t *)SPRITE_RD32(s, F_NODE);
	if (node) {
		g_setparam(node + OBJ_SHADER, "gain", v);
		g_setcolour(*(void **)(node + OBJ_SPTR), v[0], v[1], v[2], v[3]);
		for (uint32_t i = 0;; i++) {
			uint8_t *begin = (uint8_t *)SPRITE_RD32(s, NS_COMPS);
			const uint8_t *end = (const uint8_t *)SPRITE_RD32(s, NS_COMPS + 4);
			if (i >= (uint32_t)((end - begin) >> 4)) break;
			uint8_t *obj = *(uint8_t **)(begin + i * 16 + ENT_OBJ);
			g_setparam(obj + OBJ_SHADER, "gain", v);
			begin = (uint8_t *)SPRITE_RD32(s, NS_COMPS);
			obj   = *(uint8_t **)(begin + i * 16 + ENT_OBJ);
			g_setcolour(*(void **)(obj + OBJ_SPTR), v[0], v[1], v[2], v[3]);
		}
	} else {
	}
	memcpy(s + NS_GAIN, v, 16);
}

static void setalpha_c(void *self, uint32_t ab) {
	const uint32_t v[4] = { ab, ab, ab, ab };
	setgain_c(self, v);
}

static void setrgb_c(void *self, uint32_t rb, uint32_t gb, uint32_t bb) {
	const uint32_t v[4] = { rb, gb, bb, SPRITE_RD32((uint8_t *)self, NS_GAIN + 0xC) };
	setgain_c(self, v);
}

static void node_set_visible(uint8_t *obj, uint32_t vis) {
	if (!obj) return;
	uintptr_t fn = *(uintptr_t *)(*(uintptr_t *)obj + 16);
	if (fn != g_mesh_vis && fn != g_inode_vis && fn != g_branch_vis) {
		((setvis_fn)fn)(obj, vis);
		return;
	}
	uint8_t *node = (uint8_t *)SPRITE_RD32(obj, OBJ_PNODE);
	if (!node) return;
	uint8_t *ent = (uint8_t *)SPRITE_RD32(node, PN_ENTITY);
	if (!ent) return;
	SPRITE_WR8(ent, ENT_VIS, vis);
}

static int setvisible_c(void *self, uint32_t vis) {
	uint8_t *s = self;
	SPRITE_WR8(s, F_VISIBLE, vis);
	uint8_t *mesh = (uint8_t *)SPRITE_RD32(s, F_NODE);
	if (!mesh) {
		return 0;
	}
	node_set_visible(mesh, vis & (SPRITE_RD8(s, NS_LOADED) ? 0xFFFFFFFFu : 0u));
	for (uint32_t i = 0;; i++) {
		uint8_t *begin = (uint8_t *)SPRITE_RD32(s, NS_COMPS);
		const uint8_t *end = (const uint8_t *)SPRITE_RD32(s, NS_COMPS + 4);
		if (i >= (uint32_t)((end - begin) >> 4)) break;
		node_set_visible(*(uint8_t **)(begin + i * 16 + ENT_OBJ), vis);
	}
	return 0;
}

void sprite_setter_c_install(void) {
	uintptr_t pos   = so_symbol(&so_mod, SETPOS_SYM);
	uintptr_t gain  = so_symbol(&so_mod, SETGAIN_SYM);
	uintptr_t alpha = so_symbol(&so_mod, SETALPHA_SYM);
	uintptr_t rgb   = so_symbol(&so_mod, SETRGB_SYM);
	uintptr_t vis   = so_symbol(&so_mod, SETVIS_SYM);
	g_setparam  = (setparam_fn)so_symbol(&so_mod, SETPARAM_SYM);
	g_setcolour = (setcolour_fn)so_symbol(&so_mod, SETCOLOUR_SYM);
	g_mesh_vis   = so_symbol(&so_mod, MESH_VIS_SYM);
	g_inode_vis  = so_symbol(&so_mod, INODE_VIS_SYM);
	g_branch_vis = so_symbol(&so_mod, BRANCH_VIS_SYM);
	if (!pos || !gain || !alpha || !rgb || !vis || !g_setparam || !g_setcolour ||
	    !g_mesh_vis || !g_inode_vis || !g_branch_vis) {
		l_error("setter-c: a symbol is missing (pos=%p gain=%p alpha=%p rgb=%p "
		        "vis=%p setparam=%p setcolour=%p mesh_vis=%p inode_vis=%p "
		        "branch_vis=%p) - NOT installed, the setters run stock",
		        (void *)pos, (void *)gain, (void *)alpha, (void *)rgb, (void *)vis,
		        (void *)g_setparam, (void *)g_setcolour, (void *)g_mesh_vis,
		        (void *)g_inode_vis, (void *)g_branch_vis);
		return;
	}
	g_pos_hook = hook_addr(pos, (uintptr_t)&setpos_c);
	(void)hook_addr(gain,  (uintptr_t)&setgain_c);
	(void)hook_addr(alpha, (uintptr_t)&setalpha_c);
	(void)hook_addr(rgb,   (uintptr_t)&setrgb_c);
	(void)hook_addr(vis,   (uintptr_t)&setvisible_c);
	g_installed = 1;
}
