#include "utils/render_find.h"

#include <stdint.h>
#include <stddef.h>
#include <psp2/kernel/processmgr.h>
#include <so_util/so_util.h>

#include "utils/logger.h"

extern so_module so_mod;

#define FIND_SYM       "_ZN5moFlo9Rendering9CRenderer28FindRenderableObjectsInSceneEPNS_4Core6CSceneE"
#define DETERMINE_SYM  "_ZN5moFlo9Rendering9CRenderer29DetermineCurrentCullPredicateEPNS0_16CCameraComponentE"
#define CATEGORISE_SYM "_ZN5moFlo9Rendering9CRenderer21CategoriseRenderablesERKSt6vectorIPNS0_16IRenderComponentESaIS4_EE"
#define GETVIEW_SYM    "_ZN5moFlo9Rendering16CCameraComponent7GetViewEv"
#define GETPROJ_SYM    "_ZN5moFlo9Rendering16CCameraComponent13GetProjectionEv"
#define VIEWPROJ_SYM   "_ZN5moFlo9Rendering9CRenderer16matViewProjCacheE"
#define IID_CAM_SYM    "_ZN5moFlo9Rendering16CCameraComponent11InterfaceIDE"
#define IID_LIGHT_SYM  "_ZN5moFlo9Rendering15ILightComponent11InterfaceIDE"
#define IID_REND_SYM   "_ZN5moFlo9Rendering16IRenderComponent11InterfaceIDE"
#define INS_CAM_SYM    "_ZNSt6vectorIPN5moFlo9Rendering16CCameraComponentESaIS3_EE13_M_insert_auxEN9__gnu_cxx17__normal_iteratorIPS3_S5_EERKS3_"
#define INS_LIGHT_SYM  "_ZNSt6vectorIPN5moFlo9Rendering15ILightComponentESaIS3_EE13_M_insert_auxEN9__gnu_cxx17__normal_iteratorIPS3_S5_EERKS3_"
#define INS_REND_SYM   "_ZNSt6vectorIPN5moFlo9Rendering16IRenderComponentESaIS3_EE13_M_insert_auxEN9__gnu_cxx17__normal_iteratorIPS3_S5_EERKS3_"

#define RD_CAMERA       0xdc
#define RD_TRANSPARENT  0xe0
#define RD_OPAQUE       0xec
#define RD_CAMERAS      0xf8
#define RD_LIGHTS       0x104
#define SC_ENTITIES     0x4
#define ENT_COMPONENTS  0xc
#define VT_ISA          2

typedef struct { void **begin, **end, **cap; } ptr_vec;
typedef void  (*insert_aux_fn)(ptr_vec *v, void **pos, void *const *x);
typedef int   (*isa_fn)(void *comp, uint32_t iid);
typedef void  (*determine_fn)(void *rd, void *cam);
typedef void  (*categorise_fn)(void *rd, const ptr_vec *v);
typedef const float *(*getmat_fn)(void *cam);

static insert_aux_fn g_ins_cam, g_ins_light, g_ins_rend;
static determine_fn  g_determine;
static categorise_fn g_categorise;
static getmat_fn     g_get_view, g_get_proj;
static float        *g_viewproj;
static const uint32_t *g_iid_cam, *g_iid_light, *g_iid_rend;
static ptr_vec       g_rend;

static inline void vec_push(ptr_vec *v, void *p, insert_aux_fn ins) {
	if (v->end != v->cap) *v->end++ = p;
	else ins(v, v->end, &p);
}

static void find_one_walk(uint8_t *rd, uint8_t *sc) {
	ptr_vec *cams   = (ptr_vec *)(rd + RD_CAMERAS);
	ptr_vec *lights = (ptr_vec *)(rd + RD_LIGHTS);
	((ptr_vec *)(rd + RD_TRANSPARENT))->end = ((ptr_vec *)(rd + RD_TRANSPARENT))->begin;
	((ptr_vec *)(rd + RD_OPAQUE))->end      = ((ptr_vec *)(rd + RD_OPAQUE))->begin;
	lights->end = lights->begin;
	cams->end   = cams->begin;
	g_rend.end  = g_rend.begin;

	const uint32_t idc = *g_iid_cam, idl = *g_iid_light, idr = *g_iid_rend;
	void **e    = *(void ***)(sc + SC_ENTITIES);
	void **eend = *(void ***)(sc + SC_ENTITIES + 4);
	for (; e != eend; e += 2) {
		uint8_t *ent = (uint8_t *)*e;
		void **c    = *(void ***)(ent + ENT_COMPONENTS);
		void **cend = *(void ***)(ent + ENT_COMPONENTS + 4);
		for (; c != cend; c += 2) {
			void *comp = *c;
			isa_fn isa = (isa_fn)(*(uintptr_t **)comp)[VT_ISA];
			if (isa(comp, idc) != 0) vec_push(cams,    comp, g_ins_cam);
			if (isa(comp, idl) != 0) vec_push(lights,  comp, g_ins_light);
			if (isa(comp, idr) != 0) vec_push(&g_rend, comp, g_ins_rend);
		}
	}

	void *cam = cams->end != cams->begin ? cams->end[-1] : NULL;
	*(void **)(rd + RD_CAMERA) = cam;
	if (cam) {
		g_determine(rd, cam);
		g_categorise(rd, &g_rend);
		const ptr_vec *tr = (const ptr_vec *)(rd + RD_TRANSPARENT);
		if (tr->end != tr->begin) {
			const float *v = g_get_view(cam);
			const float *p = g_get_proj(cam);
			float *o = g_viewproj;
			for (int r = 0; r < 4; r++) {
				const float v0 = v[r * 4], v1 = v[r * 4 + 1], v2 = v[r * 4 + 2], v3 = v[r * 4 + 3];
				for (int k = 0; k < 4; k++)
					o[r * 4 + k] = v0 * p[k] + v1 * p[4 + k] + v2 * p[8 + k] + v3 * p[12 + k];
			}
		}
	}
}

static void one_walk_install(void) {
	uintptr_t find = so_symbol(&so_mod, FIND_SYM);
	g_determine  = (determine_fn)so_symbol(&so_mod, DETERMINE_SYM);
	g_categorise = (categorise_fn)so_symbol(&so_mod, CATEGORISE_SYM);
	g_get_view   = (getmat_fn)so_symbol(&so_mod, GETVIEW_SYM);
	g_get_proj   = (getmat_fn)so_symbol(&so_mod, GETPROJ_SYM);
	g_viewproj   = (float *)so_symbol(&so_mod, VIEWPROJ_SYM);
	g_iid_cam    = (const uint32_t *)so_symbol(&so_mod, IID_CAM_SYM);
	g_iid_light  = (const uint32_t *)so_symbol(&so_mod, IID_LIGHT_SYM);
	g_iid_rend   = (const uint32_t *)so_symbol(&so_mod, IID_REND_SYM);
	g_ins_cam    = (insert_aux_fn)so_symbol(&so_mod, INS_CAM_SYM);
	g_ins_light  = (insert_aux_fn)so_symbol(&so_mod, INS_LIGHT_SYM);
	g_ins_rend   = (insert_aux_fn)so_symbol(&so_mod, INS_REND_SYM);
	if (!find || !g_determine || !g_categorise || !g_get_view || !g_get_proj ||
	    !g_viewproj || !g_iid_cam || !g_iid_light || !g_iid_rend ||
	    !g_ins_cam || !g_ins_light || !g_ins_rend) {
		l_error("find-walk: a symbol is missing (find %d det %d cat %d view %d "
		        "proj %d vp %d iid %d%d%d ins %d%d%d) - NOT installed, find runs stock",
		        find != 0, g_determine != 0, g_categorise != 0, g_get_view != 0,
		        g_get_proj != 0, g_viewproj != 0, g_iid_cam != 0, g_iid_light != 0,
		        g_iid_rend != 0, g_ins_cam != 0, g_ins_light != 0, g_ins_rend != 0);
		return;
	}
	hook_addr(find, (uintptr_t)&find_one_walk);
}

#define CULLITEM_SYM "_ZNK5moFlo9Rendering21CFrustumCullPredicate8CullItemEPNS0_16CCameraComponentEPNS0_16IRenderComponentE"

static uint32_t cull_never(void *pred, void *cam, void *comp) {
	(void)pred; (void)cam; (void)comp;
	return 0;
}

static void no_cull_install(void) {
	uintptr_t cull = so_symbol(&so_mod, CULLITEM_SYM);
	if (!cull) {
		l_error("no-cull: " CULLITEM_SYM " not found - NOT installed, the frustum test runs stock");
		return;
	}
	hook_addr(cull, (uintptr_t)&cull_never);
}

void render_find_install(void) {
	one_walk_install();
	no_cull_install();
}
