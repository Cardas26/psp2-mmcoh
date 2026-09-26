#include "utils/render_sortprep.h"

#include <stdint.h>
#include <string.h>
#include <psp2/kernel/processmgr.h>
#include <so_util/so_util.h>

#include "utils/logger.h"

extern so_module so_mod;

#define SORTPREP_SYM "_ZN5moFlo9Rendering25CBackToFrontSortPredicate14PrepareForSortEPNS0_9CRendererEPSt6vectorIPNS0_16IRenderComponentESaIS6_EE"
#define GETXFORM_SYM "_ZN5moFlo9Rendering16IRenderComponent23GetTransformationMatrixEv"
#define VPCACHE_SYM  "_ZN5moFlo9Rendering9CRenderer16matViewProjCacheE"

#define PRED_VP       4
#define RC_SORTVALUE  0xdc

typedef const float * (*getxform_fn)(void *comp);

static getxform_fn   g_getxform;
static const float * g_vpcache;

static void sortprep_c(uint8_t *self, void *renderer, uint8_t *vec) {
	(void)renderer;
	float *vp = (float *)(self + PRED_VP);
	memcpy(vp, g_vpcache, 64);

	const float c2 = vp[2], c6 = vp[6], c10 = vp[10], c14 = vp[14];

	for (uint32_t i = 0;; i++) {
		uint8_t **begin = *(uint8_t ***)vec;
		uint8_t **end   = *(uint8_t ***)(vec + 4);
		if (i >= (uint32_t)(end - begin)) break;

		uint8_t *comp = begin[i];
		const float *w = g_getxform(comp);
		*(float *)(comp + RC_SORTVALUE) =
			((w[12] * c2 + w[13] * c6) + w[14] * c10) + w[15] * c14;
	}
}

void render_sortprep_install(void) {
	uintptr_t prep = so_symbol(&so_mod, SORTPREP_SYM);
	g_getxform = (getxform_fn)so_symbol(&so_mod, GETXFORM_SYM);
	g_vpcache  = (const float *)so_symbol(&so_mod, VPCACHE_SYM);
	if (!prep || !g_getxform || !g_vpcache) {
		l_error("sortprep-c: a symbol is missing (prep=%p getxform=%p vpcache=%p)"
		        " - NOT installed, PrepareForSort runs stock",
		        (void *)prep, (void *)g_getxform, (void *)g_vpcache);
		return;
	}
	(void)hook_addr(prep, (uintptr_t)&sortprep_c);
}
