#include "utils/render_material.h"

#include <stddef.h>
#include <string.h>
#include <psp2/kernel/processmgr.h>
#include <so_util/so_util.h>
#include <vitaGL.h>

#include "utils/logger.h"
#include "utils/render_path_profile.h"
#include "utils/render_command_cache.h"

extern so_module so_mod;

#define APPLY_SYM     "_ZN5moFlo6OpenGL13CRenderSystem13ApplyMaterialERKNS_9Rendering9CMaterialE"
#define EN_ALPHA_SYM  "_ZN5moFlo6OpenGL13CRenderSystem19EnableAlphaBlendingEb"
#define EN_DEPTHT_SYM "_ZN5moFlo6OpenGL13CRenderSystem18EnableDepthTestingEb"
#define EN_CULL_SYM   "_ZN5moFlo6OpenGL13CRenderSystem17EnableFaceCullingEb"
#define EN_COLW_SYM   "_ZN5moFlo6OpenGL13CRenderSystem19EnableColourWritingEb"
#define EN_DEPTHW_SYM "_ZN5moFlo6OpenGL13CRenderSystem18EnableDepthWritingEb"
#define EN_SCISS_SYM  "_ZN5moFlo6OpenGL13CRenderSystem20EnableScissorTestingEb"
#define BLEND_SYM     "_ZN5moFlo6OpenGL13CRenderSystem16SetBlendFunctionENS_9Rendering10AlphaBlendES3_"
#define SCISSOR_SYM   "_ZN5moFlo6OpenGL13CRenderSystem16SetScissorRegionERKNS_4Core8CVector2ES5_"
#define ATTRLOCS_SYM  "_ZN5moFlo6OpenGL13CRenderSystem21GetAttributeLocationsERKN5boost10shared_ptrINS0_7CShaderEEE"
#define UNILOCS_SYM   "_ZN5moFlo6OpenGL13CRenderSystem19GetUniformLocationsERKN5boost10shared_ptrINS0_7CShaderEEE"
#define UNILOC_SYM    "_ZN5moFlo6OpenGL7CShader18GetUniformLocationEPKc"
#define NUMUNITS_SYM  "_ZN5moFlo6OpenGL8CTexture18GetNumTextureUnitsEv"

#define MAT_SHADER        0xec
#define MAT_EMISSIVE      0xf4
#define MAT_AMBIENT       0x104
#define MAT_DIFFUSE       0x114
#define MAT_SPECULAR      0x124
#define MAT_SCISSOR_POS   0x134
#define MAT_SCISSOR_SIZE  0x13c
#define MAT_TRANSPARENT   0x158
#define MAT_COLOUR_WRITE  0x159
#define MAT_DEPTH_WRITE   0x15a
#define MAT_DEPTH_TEST    0x15b
#define MAT_CULLING       0x15c
#define MAT_SRC_BLEND     0x160
#define MAT_DST_BLEND     0x164
#define MAT_SCISSORING    0x178

#define RS_STATE_BITS     0x148
#define RS_BLEND_SRC      0x14c
#define RS_BLEND_DST      0x150
#define RS_LIGHTS         0x15c
#define RS_EMISSIVE       0x188
#define RS_AMBIENT        0x198
#define RS_DIFFUSE        0x1a8
#define RS_SPECULAR       0x1b8
#define RS_DIRTY          0x1c8
#define RS_SAMPLER_UNIT   0x304
#define RS_PROGRAM        0x308
#define RS_LOC_SAMPLER0   0x318
#define RS_LOC_SAMPLER1   0x31c
#define RS_LOC_EMISSIVE   0x320
#define RS_LOC_AMBIENT    0x324
#define RS_LOC_DIFFUSE    0x328
#define RS_LOC_SPECULAR   0x32c
#define RS_LOC_LIGHTS     0x330
#define RS_SCISSOR        0x33c
#define RS_CURRENT_MAT    0x34c
#define RS_FORCE          0x350

#define SH_PROGRAM        0x30
#define VT_BIND           0x14

typedef void     (*enable_fn)(void *rs, uint32_t on);
typedef void     (*blend_fn)(void *rs, uint32_t src, uint32_t dst);
typedef void     (*scissor_fn)(void *rs, const float *pos, const float *size);
typedef void     (*locs_fn)(void *rs, const void *shader_sp);
typedef int32_t  (*uniloc_fn)(void *shader, const char *name);
typedef uint32_t (*numunits_fn)(void);
typedef void     (*bind_fn)(void *tex, uint32_t unit);

static enable_fn   g_en_alpha, g_en_deptht, g_en_cull, g_en_colw, g_en_depthw, g_en_sciss;
static blend_fn    g_blend;
static scissor_fn  g_scissor;
static locs_fn     g_attr_locs, g_uni_locs;
static uniloc_fn   g_uniloc;
static numunits_fn g_numunits;

static inline void state(uint8_t *rs, uint8_t force, uint8_t bit, uint8_t want, enable_fn fn) {
	if (force || (((rs[RS_STATE_BITS] & bit) != 0) != (want != 0)))
		fn(rs, want != 0);
}

static inline void colour(uint8_t *rs, uint8_t force, int loc_off, int cache_off, int dirty,
                          const uint8_t *src, int specular) {
	int32_t loc = *(const int32_t *)(rs + loc_off);
	if (loc == -1)
		return;
	if (!force && rs[RS_DIRTY + dirty] && memcmp(rs + cache_off, src, 16) == 0)
		return;
	rs[RS_DIRTY + dirty] = 1;
	memcpy(rs + cache_off, src, 16);
	const float *c = (const float *)src;
	RPP(glUniform4f)(loc, c[0], c[1], c[2], specular ? 1.0f / c[3] : c[3]);
}

static uint32_t apply_material_c(uint8_t *rs, uint8_t *mat) {
	uint8_t force = rs[RS_FORCE];
	if (!force && *(uint8_t **)(rs + RS_CURRENT_MAT) == mat && mat[MAT_CACHE_VALID])
		goto done;
	*(uint8_t **)(rs + RS_CURRENT_MAT) = mat;

	uint8_t transparent = mat[MAT_TRANSPARENT];
	state(rs, force, 0x01, transparent, g_en_alpha);
	if (transparent) {
		uint32_t src = *(const uint32_t *)(mat + MAT_SRC_BLEND);
		uint32_t dst = *(const uint32_t *)(mat + MAT_DST_BLEND);
		if (force || src != *(const uint32_t *)(rs + RS_BLEND_SRC) ||
		    dst != *(const uint32_t *)(rs + RS_BLEND_DST))
			g_blend(rs, src, dst);
	}
	state(rs, force, 0x08, mat[MAT_COLOUR_WRITE], g_en_colw);
	state(rs, force, 0x10, mat[MAT_DEPTH_WRITE],  g_en_depthw);
	state(rs, force, 0x04, mat[MAT_CULLING],      g_en_cull);
	state(rs, force, 0x02, mat[MAT_DEPTH_TEST],   g_en_deptht);
	state(rs, force, 0x20, mat[MAT_SCISSORING],   g_en_sciss);
	if (force || memcmp(rs + RS_SCISSOR, mat + MAT_SCISSOR_POS, 8) != 0 ||
	    memcmp(rs + RS_SCISSOR + 8, mat + MAT_SCISSOR_SIZE, 8) != 0)
		g_scissor(rs, (const float *)(mat + MAT_SCISSOR_POS), (const float *)(mat + MAT_SCISSOR_SIZE));

	uint8_t *shader = *(uint8_t **)(mat + MAT_SHADER);
	if (shader) {
		uint32_t prog = *(const uint32_t *)(shader + SH_PROGRAM);
		if (prog) {
			if (force || prog != *(const uint32_t *)(rs + RS_PROGRAM)) {
				RPP(glUseProgram)(prog);
				*(uint32_t *)(rs + RS_PROGRAM) = prog;
				g_attr_locs(rs, mat + MAT_SHADER);
				g_uni_locs(rs, mat + MAT_SHADER);
				*(uint32_t *)(rs + RS_DIRTY) = 0;
			}

			for (int m = 0; m < MAT_MAP_COUNT; m++) {
				const uint8_t *map = mat + MAT_MAP_FIRST + m * MAT_MAP_STRIDE;
				if (*(const uint32_t *)(map + MAT_MAP_SIZE_OFF) == 0)
					continue;
				const uint32_t *const *bucket = *(const uint32_t *const **)(map + MAT_MAP_BUCKETS_OFF);
				if (!bucket)
					continue;
				const uint32_t *node = (const uint32_t *)*bucket;
				while (node) {
					int32_t loc = g_uniloc(shader, (const char *)node[1]);
					const float *v = (const float *)(node + 2);
					switch (m) {
					case 0: RPP(glUniform1f)(loc, v[0]); break;
					case 1: RPP(glUniform2fv)(loc, 1, v); break;
					case 2: RPP(glUniform3fv)(loc, 1, v); break;
					case 3: RPP(glUniform4fv)(loc, 1, v); break;
					case 4: RPP(glUniformMatrix4fv)(loc, 1, GL_FALSE, v); break;
					case 5: {
						const float *b = (const float *)node[2], *e = (const float *)node[3];
						RPP(glUniformMatrix4fv)(loc, (GLsizei)(((const uint8_t *)e - (const uint8_t *)b) >> 6), GL_FALSE, b);
						break;
					}
					default: RPP(glUniform4fv)(loc, 1, v); break;
					}
					node = (const uint32_t *)node[0];
					while (!node) {
						bucket++;
						node = (const uint32_t *)*bucket;
					}
					if (node == (const uint32_t *)bucket)
						node = NULL;
				}
			}

			const uint32_t *tb = *(const uint32_t *const *)(mat + MAT_TEXTURES);
			const uint32_t *te = *(const uint32_t *const *)(mat + MAT_TEXTURES + 4);
			uint32_t ntex = (uint32_t)(te - tb) / 2;
			if (ntex == 0 || tb[0] != 0) {
				if (ntex) {
					uint32_t units = g_numunits();
					for (uint32_t i = 0; i < ntex && i < units; i++) {
						uint8_t *tex = (uint8_t *)tb[2 * i];
						if (!tex) continue;
						bind_fn bind = *(bind_fn *)(*(uintptr_t *)tex + VT_BIND);
						bind(tex, i);
					}
				}
				int32_t s0 = *(const int32_t *)(rs + RS_LOC_SAMPLER0);
				if (s0 != -1 && (force || *(const uint32_t *)(rs + RS_SAMPLER_UNIT) != 0)) {
					*(uint32_t *)(rs + RS_SAMPLER_UNIT) = 0;
					RPP(glUniform1i)(s0, 0);
				}
				int32_t s1 = *(const int32_t *)(rs + RS_LOC_SAMPLER1);
				if (s1 != -1 && (force || *(const uint32_t *)(rs + RS_SAMPLER_UNIT) != 1)) {
					*(uint32_t *)(rs + RS_SAMPLER_UNIT) = 1;
					RPP(glUniform1i)(s1, 1);
				}
			}

			colour(rs, force, RS_LOC_EMISSIVE, RS_EMISSIVE, 0, mat + MAT_EMISSIVE, 0);
			colour(rs, force, RS_LOC_AMBIENT,  RS_AMBIENT,  1, mat + MAT_AMBIENT,  0);
			colour(rs, force, RS_LOC_DIFFUSE,  RS_DIFFUSE,  2, mat + MAT_DIFFUSE,  0);
			colour(rs, force, RS_LOC_SPECULAR, RS_SPECULAR, 3, mat + MAT_SPECULAR, 1);
			int32_t lloc = *(const int32_t *)(rs + RS_LOC_LIGHTS);
			if (lloc >= 0) {
				const uint8_t *lb = *(const uint8_t *const *)(rs + RS_LIGHTS);
				const uint8_t *le = *(const uint8_t *const *)(rs + RS_LIGHTS + 4);
				if (lb != le)
					RPP(glUniform4fv)(lloc, (GLsizei)((le - lb) >> 4), (const float *)lb);
			}
		}
	}
	mat[MAT_CACHE_VALID] = 1;
done:
	return 0;
}

void render_material_install(void) {
	uintptr_t apply = so_symbol(&so_mod, APPLY_SYM);
	g_en_alpha  = (enable_fn)so_symbol(&so_mod, EN_ALPHA_SYM);
	g_en_deptht = (enable_fn)so_symbol(&so_mod, EN_DEPTHT_SYM);
	g_en_cull   = (enable_fn)so_symbol(&so_mod, EN_CULL_SYM);
	g_en_colw   = (enable_fn)so_symbol(&so_mod, EN_COLW_SYM);
	g_en_depthw = (enable_fn)so_symbol(&so_mod, EN_DEPTHW_SYM);
	g_en_sciss  = (enable_fn)so_symbol(&so_mod, EN_SCISS_SYM);
	g_blend     = (blend_fn)so_symbol(&so_mod, BLEND_SYM);
	g_scissor   = (scissor_fn)so_symbol(&so_mod, SCISSOR_SYM);
	g_attr_locs = (locs_fn)so_symbol(&so_mod, ATTRLOCS_SYM);
	g_uni_locs  = (locs_fn)so_symbol(&so_mod, UNILOCS_SYM);
	g_uniloc    = (uniloc_fn)so_symbol(&so_mod, UNILOC_SYM);
	g_numunits  = (numunits_fn)so_symbol(&so_mod, NUMUNITS_SYM);
	if (!apply || !g_en_alpha || !g_en_deptht || !g_en_cull || !g_en_colw || !g_en_depthw ||
	    !g_en_sciss || !g_blend || !g_scissor || !g_attr_locs || !g_uni_locs || !g_uniloc ||
	    !g_numunits) {
		l_error("material-c: a symbol is missing - NOT installed, ApplyMaterial runs stock");
		return;
	}
	hook_addr(apply, (uintptr_t)&apply_material_c);
}
