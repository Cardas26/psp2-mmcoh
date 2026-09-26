#include "utils/transform_cheap_push.h"

#include <stdint.h>
#include <string.h>
#include <so_util/so_util.h>

#include "utils/logger.h"

extern so_module so_mod;

#define SET_TRANSFORM_SYM \
	"_ZN5INode12setTransformERKN5moFlo4Core10CMatrix4x4E"
#define SET_LOCAL_SYM \
	"_ZN5moFlo4Core10CTransform17SetLocalTransformERKNS0_10CMatrix4x4E"
#define ON_CHANGED_SYM \
	"_ZN5moFlo4Core10CTransform18OnTransformChangedEv"
#define ENTITY_TRANSFORM_SYM "_ZN5moFlo4Core7CEntity9TransformEv"
#define SC_COPY_SYM          "_ZN5boost6detail12shared_countC1ERKS1_"
#define SC_DTOR_SYM          "_ZN5boost6detail12shared_countD1Ev"

#define NODE_HOLDER   0x04
#define HOLDER_ENTITY 0x04
#define HOLDER_COUNT  0x08

#define T_MATRIX_BYTES 64
#define T_DECOMP       0x80
#define T_DECOMP_WORDS 10
#define T_DECOMP_BYTES (T_DECOMP_WORDS * 4)

typedef uint32_t (*so_set_transform_fn)(void *node, const void *m);
typedef void     (*so_set_local_fn)(void *t, const void *m);
typedef void     (*so_on_changed_fn)(void *t);
typedef void *   (*so_entity_transform_fn)(void *entity);
typedef void     (*so_sc_copy_fn)(void *dst, const void *src);
typedef void     (*so_sc_dtor_fn)(void *pn);

static so_set_local_fn        g_set_local;
static so_on_changed_fn       g_on_changed;
static so_entity_transform_fn g_entity_transform;
static so_sc_copy_fn          g_sc_copy;
static so_sc_dtor_fn          g_sc_dtor;

#define TAB_BITS  10
#define TAB_SIZE  (1u << TAB_BITS)
#define TAB_PROBE 32

static struct {
	void *key;
	uint32_t decomp[T_DECOMP_WORDS];
} g_tab[TAB_SIZE];

#define GATE_ON() (1)

static int tab_index(void *key, uint32_t *out) {
	uint32_t h = ((uint32_t)(uintptr_t)key * 2654435761u) >> (32 - TAB_BITS);
	for (uint32_t i = 0; i < TAB_PROBE; i++) {
		uint32_t j = (h + i) & (TAB_SIZE - 1);
		if (g_tab[j].key == key || !g_tab[j].key) {
			g_tab[j].key = key;
			*out = j;
			return 1;
		}
	}
	return 0;
}

static int cheap_ok(const void *t, const void *m, uint32_t row) {
	return memcmp(m, t, T_MATRIX_BYTES) == 0 &&
	       memcmp((const uint8_t *)t + T_DECOMP, g_tab[row].decomp,
	              T_DECOMP_BYTES) == 0;
}

static void snap(const void *t, uint32_t row) {
	memcpy(g_tab[row].decomp, (const uint8_t *)t + T_DECOMP, T_DECOMP_BYTES);
}

static uint32_t w_set_transform(void *node, const void *m) {

	void *holder = *(void **)((uint8_t *)node + NODE_HOLDER);
	if (!holder) {
		return 0;
	}

	void *entity = *(void **)((uint8_t *)holder + HOLDER_ENTITY);

	void *pn = NULL;
	g_sc_copy(&pn, (const uint8_t *)holder + HOLDER_COUNT);

	void *t = g_entity_transform(entity);

	uint32_t row = 0;
	int have_row = tab_index(t, &row);

	if (GATE_ON() && have_row && cheap_ok(t, m, row)) {
		g_on_changed(t);
	} else {
		g_set_local(t, m);
		if (have_row) snap(t, row);
	}

	g_sc_dtor(&pn);
	return 0;
}

void transform_cheap_push_install(void) {
	uintptr_t set_transform = so_symbol(&so_mod, SET_TRANSFORM_SYM);
	g_set_local        = (so_set_local_fn)so_symbol(&so_mod, SET_LOCAL_SYM);
	g_on_changed       = (so_on_changed_fn)so_symbol(&so_mod, ON_CHANGED_SYM);
	g_entity_transform =
		(so_entity_transform_fn)so_symbol(&so_mod, ENTITY_TRANSFORM_SYM);
	g_sc_copy          = (so_sc_copy_fn)so_symbol(&so_mod, SC_COPY_SYM);
	g_sc_dtor          = (so_sc_dtor_fn)so_symbol(&so_mod, SC_DTOR_SYM);

	if (!set_transform || !g_set_local || !g_on_changed || !g_entity_transform ||
	    !g_sc_copy || !g_sc_dtor) {
		l_error("[cheap-push] a symbol is missing (setTransform=%p setLocal=%p "
		        "onChanged=%p entTransform=%p sccopy=%p scdtor=%p) - NOT "
		        "installed, INode::setTransform runs stock",
		        (void *)set_transform, (void *)g_set_local,
		        (void *)g_on_changed, (void *)g_entity_transform,
		        (void *)g_sc_copy, (void *)g_sc_dtor);
		g_set_local = NULL;
		return;
	}

	hook_addr(set_transform, (uintptr_t)&w_set_transform);
}
