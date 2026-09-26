#include "utils/palette_info_memo.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <so_util/so_util.h>

#include "utils/lazy_lwmutex.h"
#include "utils/logger.h"
#include "utils/update_profile.h"

extern so_module so_mod;

#define PALREAD_SYM   "_ZN19TextureManagerUtils15ReadPaletteInfoERKSsRSsRi"
#define STRASSIGN_SYM "_ZNSs6assignIN9__gnu_cxx17__normal_iteratorIPKcSsEEEERSsT_S6_"

typedef void *(*str_assign_fn)(void *self, const char *first, const char *last);

static so_hook       g_hook;
static str_assign_fn g_assign;

#define PIM_ROWS    32
#define PIM_KEY_MAX 127
#define PIM_VAL_MAX 79

typedef struct {
	uint32_t hash;
	uint32_t klen;
	uint32_t vlen;
	int      idx;
	char     key[PIM_KEY_MAX + 1];
	char     val[PIM_VAL_MAX + 1];
} pim_row;

static pim_row        g_rows[PIM_ROWS];
static uint32_t       g_used;
static lazy_lwmutex_t g_lock = LAZY_LWMUTEX_INITIALIZER;

#define ARM_ON() (1)

static const char *pim_str(const void *s, uint32_t *len_out, uint32_t cap)
{
	if (!s)
		return NULL;
	const char *d = *(const char *const *)s;
	if (!d)
		return NULL;
	uint32_t len = ((const uint32_t *)d)[-3];
	if (len > cap)
		return NULL;
	*len_out = len;
	return d;
}

static uint32_t pim_hash(const char *p, uint32_t n)
{
	uint32_t h = 2166136261u;
	for (uint32_t i = 0; i < n; i++) {
		h ^= (uint8_t)p[i];
		h *= 16777619u;
	}
	return h;
}

static uint64_t palette_read_c(void *in, void *out_s, void *out_i)
{
#define PIM_DONE() ((void)0)

	if (!ARM_ON()) {
		uint64_t r = SO_CONTINUE(uint64_t, g_hook, in, out_s, out_i);
		return r;
	}

	uint32_t klen = 0;
	const char *k = pim_str(in, &klen, PIM_KEY_MAX);
	if (!k || !out_s || !out_i) {
		uint64_t r = SO_CONTINUE(uint64_t, g_hook, in, out_s, out_i);
		return r;
	}

	uint32_t h = pim_hash(k, klen);

	char     val[PIM_VAL_MAX + 1];
	uint32_t vlen = 0;
	int      idx  = -1;
	bool     hit  = false;

	if (lazy_lwmutex_lock(&g_lock, "palmemo_lock")) {
		for (uint32_t i = 0; i < g_used; i++) {
			const pim_row *row = &g_rows[i];
			if (row->hash == h && row->klen == klen &&
			    memcmp(row->key, k, klen) == 0) {
				memcpy(val, row->val, row->vlen + 1);
				vlen = row->vlen;
				idx  = row->idx;
				hit  = true;
				break;
			}
		}
		lazy_lwmutex_unlock(&g_lock);
	}

	if (hit) {
		g_assign(out_s, val, val + vlen);
		*(int *)out_i = idx;
		return 0;
	}

	uint64_t r = SO_CONTINUE(uint64_t, g_hook, in, out_s, out_i);

	uint32_t alen = 0;
	const char *a = pim_str(out_s, &alen, PIM_VAL_MAX);
	if (!a || alen == 0) {
		return r;
	}

	if (lazy_lwmutex_lock(&g_lock, "palmemo_lock")) {
		bool known = false;
		for (uint32_t i = 0; i < g_used; i++) {
			if (g_rows[i].hash == h && g_rows[i].klen == klen &&
			    memcmp(g_rows[i].key, k, klen) == 0) {
				known = true;
				break;
			}
		}
		if (!known) {
			if (g_used < PIM_ROWS) {
				pim_row *row = &g_rows[g_used];
				row->hash = h;
				row->klen = klen;
				row->vlen = alen;
				row->idx  = *(const int *)out_i;
				memcpy(row->key, k, klen);
				row->key[klen] = '\0';
				memcpy(row->val, a, alen);
				row->val[alen] = '\0';
				g_used++;
			} else {
			}
		}
		lazy_lwmutex_unlock(&g_lock);
	}

	return r;
#undef PIM_DONE
}

void palette_info_memo_install(void)
{
	uintptr_t read = so_symbol(&so_mod, PALREAD_SYM);
	g_assign = (str_assign_fn)so_symbol(&so_mod, STRASSIGN_SYM);

	if (!read || !g_assign) {
		l_error("palmemo: a symbol is missing (ReadPaletteInfo=%p "
		        "string::assign=%p) - NOT installed, the parse runs stock",
		        (void *)read, (void *)g_assign);
		return;
	}

	g_hook = hook_addr(read, (uintptr_t)&palette_read_c);
}
