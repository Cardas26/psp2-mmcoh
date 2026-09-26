#include "utils/render_rbuf.h"

#include <stddef.h>
#include <string.h>
#include <psp2/kernel/processmgr.h>
#include <so_util/so_util.h>
#include <vitaGL.h>

#include "utils/logger.h"
#include "utils/render_path_profile.h"

extern so_module so_mod;

#define RBUF_SYM     "_ZN5moFlo6OpenGL13CRenderSystem12RenderBufferEPNS_9Rendering11IMeshBufferEjjRKNS_4Core10CMatrix4x4E"
#define EVA_SYM      "_ZN5moFlo6OpenGL13CRenderSystem32EnableVertexAttributeForSemanticEPNS_9Rendering11IMeshBufferE"
#define PRIM_SYM     "_ZNK5moFlo9Rendering11IMeshBuffer16GetPrimitiveTypeEv"
#define RSPRIM_SYM   "_ZN5moFlo6OpenGL13CRenderSystem16GetPrimitiveTypeENS_9Rendering13PrimitiveTypeE"
#define INVERSE_SYM  "_ZNK5moFlo4Core10CMatrix4x47InverseEv"
#define TRANSP_SYM   "_ZNK5moFlo4Core10CMatrix4x412GetTransposeEv"

#define RS_VIEWPROJ    0x20c
#define RS_NATTRIB     0x2f8
#define RS_PROGRAM     0x308
#define RS_LOC_WVP     0x30c
#define RS_LOC_WORLD   0x310
#define RS_LOC_NORMAL  0x314
#define RS_FORCE       0x350
#define MB_CACHE_VALID 0x71

typedef uint32_t (*prim_fn)(const void *mb);
typedef uint32_t (*rsprim_fn)(void *rs, uint32_t prim);
typedef void     (*mat_fn)(float *out, const float *self);

static prim_fn   g_prim;
static rsprim_fn g_rsprim;
static mat_fn    g_inverse, g_transpose;
static so_hook   g_eva_hook;
static uintptr_t g_eva_addr;

static float g_world[16], g_vp[16], g_wvp[16];
static int   g_have;

static const void *g_walk_buf;
static uint32_t    g_walk_prog;

static void mul4(const float *a, const float *b, float *o) {
	for (int r = 0; r < 4; r++) {
		const float a0 = a[r * 4], a1 = a[r * 4 + 1], a2 = a[r * 4 + 2], a3 = a[r * 4 + 3];
		for (int k = 0; k < 4; k++)
			o[r * 4 + k] = a0 * b[k] + a1 * b[4 + k] + a2 * b[8 + k] + a3 * b[12 + k];
	}
}

static uint32_t render_buffer_c(uint8_t *rs, uint8_t *mb, uint32_t offset, uint32_t nidx, const float *world) {
	const float *vp = (const float *)(rs + RS_VIEWPROJ);
	if (!g_have || memcmp(world, g_world, 64) != 0 || memcmp(vp, g_vp, 64) != 0) {
		memcpy(g_world, world, 64);
		memcpy(g_vp, vp, 64);
		mul4(g_world, g_vp, g_wvp);
		g_have = 1;
	}
	RPP(glUniformMatrix4fv)(*(const int32_t *)(rs + RS_LOC_WVP), 1, GL_FALSE, g_wvp);
	int32_t loc = *(const int32_t *)(rs + RS_LOC_WORLD);
	if (loc != -1)
		RPP(glUniformMatrix4fv)(loc, 1, GL_FALSE, world);
	loc = *(const int32_t *)(rs + RS_LOC_NORMAL);
	if (loc != -1) {
		float inv[16], tr[16];
		g_inverse(inv, world);
		g_transpose(tr, inv);
		RPP(glUniformMatrix4fv)(loc, 1, GL_FALSE, tr);
	}
	((void (*)(void *, void *))g_eva_addr)(rs, mb);
	uint32_t prim = g_prim(mb);
	GLenum mode;
	switch (prim) {
	case 0:  mode = GL_TRIANGLES;      break;
	case 1:  mode = GL_TRIANGLE_STRIP; break;
	case 2:  mode = GL_LINES;          break;
	default: mode = (GLenum)g_rsprim(rs, prim); break;
	}
	RPP(glDrawElements)(mode, (GLsizei)nidx, GL_UNSIGNED_SHORT, (const void *)(uintptr_t)offset);
	rs[RS_FORCE] = 0;
	return 0;
}

static uint32_t eva_gate(uint8_t *rs, uint8_t *mb) {
	uint32_t prog = *(const uint32_t *)(rs + RS_PROGRAM);
	if (!rs[RS_FORCE] && *(const uint32_t *)(rs + RS_NATTRIB) != 0 && mb[MB_CACHE_VALID] &&
	    (const void *)mb == g_walk_buf && prog == g_walk_prog) {
		return 0;
	}
	uint32_t r = SO_CONTINUE(uint32_t, g_eva_hook, rs, mb);
	g_walk_buf = mb;
	g_walk_prog = *(const uint32_t *)(rs + RS_PROGRAM);
	return r;
}

void render_rbuf_install(void) {
	uintptr_t rbuf = so_symbol(&so_mod, RBUF_SYM);
	g_eva_addr  = so_symbol(&so_mod, EVA_SYM);
	g_prim      = (prim_fn)so_symbol(&so_mod, PRIM_SYM);
	g_rsprim    = (rsprim_fn)so_symbol(&so_mod, RSPRIM_SYM);
	g_inverse   = (mat_fn)so_symbol(&so_mod, INVERSE_SYM);
	g_transpose = (mat_fn)so_symbol(&so_mod, TRANSP_SYM);
	if (!rbuf || !g_eva_addr || !g_prim || !g_rsprim || !g_inverse || !g_transpose) {
		l_error("rbuf-c: a symbol is missing - NOT installed, RenderBuffer runs stock");
		return;
	}
	g_eva_hook = hook_addr(g_eva_addr, (uintptr_t)&eva_gate);
	hook_addr(rbuf, (uintptr_t)&render_buffer_c);
}
