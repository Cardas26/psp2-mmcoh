#include "utils/render_sprdata.h"

#include <stddef.h>
#include <string.h>
#include <psp2/kernel/processmgr.h>
#include <so_util/so_util.h>

#include "utils/logger.h"

extern so_module so_mod;

#define SPRDATA_SYM "_ZN5moFlo9Rendering16CSpriteComponent19CalculateSpriteDataEv"
#define CORNERS_SYM "_ZN5moFlo9Rendering16CSpriteComponent24CalculateCornerPositionsEv"
#define FRAME_SYM   "_ZN5moFlo9Rendering16CSpriteComponent15GetCurrentFrameEv"
#define TL_SYM      "_ZNK5moFlo4Core9Rectangle7TopLeftEv"
#define BL_SYM      "_ZNK5moFlo4Core9Rectangle10BottomLeftEv"
#define TR_SYM      "_ZNK5moFlo4Core9Rectangle8TopRightEv"
#define BR_SYM      "_ZNK5moFlo4Core9Rectangle11BottomRightEv"
#define SC_COPY_SYM "_ZN5boost6detail12shared_countC1ERKS1_"
#define SC_DTOR_SYM "_ZN5boost6detail12shared_countD1Ev"

#define SD_MATERIAL     0xd4
#define SD_VERTS        0xe4
#define SD_VERT_STRIDE  28
#define SD_VERT_UV      16
#define SD_VERT_COLOUR  24
#define SD_SD_MATERIAL  0x154
#define SD_CORNER       0x19c
#define SD_COLOUR       0x224
#define SD_CORNERS_OK   0x22e
#define SD_UVS_OK       0x22f

typedef void  (*void_fn)(void *self);
typedef void *(*frame_fn)(void *self);
typedef void *(*tl_fn)(const void *rect);
typedef void  (*corner_fn)(void *ret, const void *rect);
typedef void  (*sc_copy_fn)(void *dst, const void *src);
typedef void  (*sc_dtor_fn)(void *dst);

static void_fn    g_corners;
static frame_fn   g_frame;
static tl_fn      g_topleft;
static corner_fn  g_bottomleft, g_topright, g_bottomright;
static sc_copy_fn g_sc_copy;
static sc_dtor_fn g_sc_dtor;

static uint32_t calculate_sprite_data_c(uint8_t *sp) {
	uint8_t *vert = sp + SD_VERTS;

	if (!sp[SD_CORNERS_OK]) {
		g_corners(sp);
		for (int i = 0; i < 4; i++)
			memcpy(vert + i * SD_VERT_STRIDE, sp + SD_CORNER + i * 16, 16);
	}

	void **mat = (void **)(sp + SD_MATERIAL);
	void **sdm = (void **)(sp + SD_SD_MATERIAL);
	if (mat[0] != sdm[0] || mat[1] != sdm[1]) {
		void *tmp[2];
		tmp[0] = mat[0];
		tmp[1] = NULL;
		g_sc_copy(&tmp[1], &mat[1]);
		void *old_px = sdm[0], *old_pn = sdm[1];
		sdm[0] = tmp[0];
		sdm[1] = tmp[1];
		tmp[0] = old_px;
		tmp[1] = old_pn;
		g_sc_dtor(&tmp[1]);
	}

	uint32_t col = *(const uint32_t *)(sp + SD_COLOUR);
	for (int i = 0; i < 4; i++)
		*(uint32_t *)(vert + i * SD_VERT_STRIDE + SD_VERT_COLOUR) = col;

	if (!sp[SD_UVS_OK]) {
		sp[SD_UVS_OK] = 1;
		uint32_t rect[4];
		memcpy(rect, g_frame(sp), sizeof(rect));
		memcpy(vert + SD_VERT_UV, g_topleft(rect), 8);
		g_bottomleft(vert + 1 * SD_VERT_STRIDE + SD_VERT_UV, rect);
		g_topright(vert + 2 * SD_VERT_STRIDE + SD_VERT_UV, rect);
		g_bottomright(vert + 3 * SD_VERT_STRIDE + SD_VERT_UV, rect);
	}

	return 0;
}

void render_sprdata_install(void) {
	uintptr_t sprdata = so_symbol(&so_mod, SPRDATA_SYM);
	g_corners      = (void_fn)so_symbol(&so_mod, CORNERS_SYM);
	g_frame        = (frame_fn)so_symbol(&so_mod, FRAME_SYM);
	g_topleft      = (tl_fn)so_symbol(&so_mod, TL_SYM);
	g_bottomleft   = (corner_fn)so_symbol(&so_mod, BL_SYM);
	g_topright     = (corner_fn)so_symbol(&so_mod, TR_SYM);
	g_bottomright  = (corner_fn)so_symbol(&so_mod, BR_SYM);
	g_sc_copy      = (sc_copy_fn)so_symbol(&so_mod, SC_COPY_SYM);
	g_sc_dtor      = (sc_dtor_fn)so_symbol(&so_mod, SC_DTOR_SYM);
	if (!sprdata || !g_corners || !g_frame || !g_topleft || !g_bottomleft ||
	    !g_topright || !g_bottomright || !g_sc_copy || !g_sc_dtor) {
		l_error("sprdata-c: a symbol is missing - NOT installed, CalculateSpriteData runs stock");
		return;
	}
	hook_addr(sprdata, (uintptr_t)&calculate_sprite_data_c);
}
