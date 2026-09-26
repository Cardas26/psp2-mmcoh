#include "utils/render_dynrend.h"

#include <stdint.h>
#include <psp2/kernel/processmgr.h>
#include <so_util/so_util.h>

#include "utils/logger.h"

extern so_module so_mod;

#define DYNREND_SYM   "_ZN5moFlo9Rendering19CDynamicSpriteBatch6RenderEPNS0_13IRenderSystemERKNS0_16CSpriteComponent10SpriteDataEPKNS_4Core10CMatrix4x4E"
#define FORCERENDER_SYM "_ZN5moFlo9Rendering19CDynamicSpriteBatch11ForceRenderEPNS0_13IRenderSystemE"
#define FORCECMD_SYM  "_ZN5moFlo9Rendering19CDynamicSpriteBatch18ForceCommandChangeEv"
#define SDCOPY_SYM    "_ZN5moFlo9Rendering16CSpriteComponent10SpriteDataC1ERKS2_"
#define INSERTAUX_SYM "_ZNSt6vectorIN5moFlo9Rendering16CSpriteComponent10SpriteDataESaIS3_EE13_M_insert_auxEN9__gnu_cxx17__normal_iteratorIPS3_S5_EERKS3_"
#define MATASSIGN_SYM "_ZN5boost10shared_ptrIN5moFlo9Rendering9CMaterialEEaSERKS4_"

#define DSB_SPRITES    0x08
#define DSB_END        0x0c
#define DSB_CAP        0x10
#define DSB_MATERIAL   0x20
#define DSB_COUNT      0x2c
#define DSB_MAX_SPRITES 2047

#define SD_SIZE        0x78
#define SD_VERT        0x1c
#define SD_MATERIAL    0x70

typedef void (*forcerender_fn)(void *self, void *rs);
typedef void (*forcecmd_fn)(void *self);
typedef void (*sdcopy_fn)(void *dst, const void *src);
typedef void (*insertaux_fn)(void *vec, void *pos, const void *val);
typedef void (*matassign_fn)(void *dst, const void *src);

static forcerender_fn g_force_render;
static uintptr_t      g_force_cmd;
static sdcopy_fn      g_sd_copy;
static insertaux_fn   g_insert_aux;
static matassign_fn   g_mat_assign;

static void dynrend_c(uint8_t *self, void *rs, const uint8_t *sprite, const float *m) {
	{
		const uint8_t *begin = *(const uint8_t **)(self + DSB_SPRITES);
		const uint8_t *end   = *(const uint8_t **)(self + DSB_END);
		if ((uint32_t)(end - begin) / SD_SIZE > DSB_MAX_SPRITES)
			g_force_render(self, rs);
	}

	{
		const void *cur = *(void *const *)(self + DSB_MATERIAL);
		if (cur && cur != *(const void *const *)(sprite + SD_MATERIAL))
			((forcecmd_fn)g_force_cmd)(self);
	}

	{
		uint8_t *end = *(uint8_t **)(self + DSB_END);
		if (end == *(uint8_t **)(self + DSB_CAP)) {
			g_insert_aux(self + DSB_SPRITES, end, sprite);
		} else {
			if (end)
				g_sd_copy(end, sprite);
			*(uint8_t **)(self + DSB_END) = *(uint8_t **)(self + DSB_END) + SD_SIZE;
		}
	}

	if (m) {
		float *dst = (float *)(*(uint8_t **)(self + DSB_END) - SD_SIZE);
		const float *src = (const float *)sprite;
		for (int v = 0; v < 4; v++, src = (const float *)((const uint8_t *)src + SD_VERT),
		                           dst = (float *)((uint8_t *)dst + SD_VERT)) {
			const float s0 = src[0], s1 = src[1], s2 = src[2], s3 = src[3];
			dst[0] = ((s0 * m[0] + s1 * m[4]) + s2 * m[8])  + s3 * m[12];
			dst[1] = ((s0 * m[1] + s1 * m[5]) + s2 * m[9])  + s3 * m[13];
			dst[2] = ((s0 * m[2] + s1 * m[6]) + s2 * m[10]) + s3 * m[14];
			dst[3] = ((s0 * m[3] + s1 * m[7]) + s2 * m[11]) + s3 * m[15];
		}
	}

	if (*(void **)(self + DSB_MATERIAL) != *(void *const *)(sprite + SD_MATERIAL) ||
	    *(void **)(self + DSB_MATERIAL + 4) != *(void *const *)(sprite + SD_MATERIAL + 4)) {
		g_mat_assign(self + DSB_MATERIAL, sprite + SD_MATERIAL);
	}

	*(uint32_t *)(self + DSB_COUNT) += 1;
}

void render_dynrend_install(void) {
	uintptr_t render = so_symbol(&so_mod, DYNREND_SYM);
	g_force_render = (forcerender_fn)so_symbol(&so_mod, FORCERENDER_SYM);
	g_force_cmd    = so_symbol(&so_mod, FORCECMD_SYM);
	g_sd_copy      = (sdcopy_fn)so_symbol(&so_mod, SDCOPY_SYM);
	g_insert_aux   = (insertaux_fn)so_symbol(&so_mod, INSERTAUX_SYM);
	g_mat_assign   = (matassign_fn)so_symbol(&so_mod, MATASSIGN_SYM);
	if (!render || !g_force_render || !g_force_cmd || !g_sd_copy || !g_insert_aux || !g_mat_assign) {
		l_error("dynrend-c: a symbol is missing (render=%p forcerender=%p forcecmd=%p "
		        "sdcopy=%p insertaux=%p matassign=%p) - NOT installed, Render runs stock",
		        (void *)render, (void *)g_force_render, (void *)g_force_cmd,
		        (void *)g_sd_copy, (void *)g_insert_aux, (void *)g_mat_assign);
		return;
	}
	(void)hook_addr(render, (uintptr_t)&dynrend_c);
}
