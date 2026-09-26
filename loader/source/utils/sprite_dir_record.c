#include "utils/sprite_dir_record.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <psp2/kernel/processmgr.h>
#include <so_util/so_util.h>

#include "utils/lazy_lwmutex.h"
#include "utils/logger.h"
#include "utils/update_profile.h"

extern so_module so_mod;

#define CTL_SYM    "_ZN15AnimationSystem16createControllerEPKcP9NewSpriteP8QuadMesh"
#define PAL_SYM    "_ZN19TextureManagerUtils18HasPaletteInfoFileERKSs"
#define NRM_SYM    "_ZN9NewSprite24getNormalTextureFileNameEPKc"
#define SETUP_SYM  "_ZN16SpriteController5setupEPN20IAnimationController15t_animationListEPNS_8t_coordsEPNS_8t_soundsEP9NewSpriteP8QuadMesh"
#define STATE_SYM  "_ZN16SpriteController8setStateEii"
#define EXISTS_SYM "_ZN7IEngine10fileExistsEPKc"

static const char *const k_file_sym[3] = {
	"_ZN15AnimationSystem22ANIMSYS_ANIMATION_FILEE",
	"_ZN15AnimationSystem24ANIMSYS_COORDINATES_FILEE",
	"_ZN15AnimationSystem18ANIMSYS_SOUND_FILEE",
};
static const char *const k_pool_sym[3] = {
	"_ZN15AnimationSystem12m_animationsE",
	"_ZN15AnimationSystem8m_coordsE",
	"_ZN15AnimationSystem8m_soundsE",
};
#define CTLPOOL_SYM "_ZN15AnimationSystem13m_controllersE"

typedef struct {
	uint32_t cap;
	uint32_t used;
	uint32_t unk;
	uint8_t **items;
} sdr_pool;

#define REF_COUNT(r)   (*(int32_t *)(r))
#define REF_NAME(r)    (*(const char *const *)((r) + 4))
#define REF_PAYLOAD(r) ((void *)((r) + 8))

typedef void *(*ctl_fn)(const char *dir, void *sprite, void *mesh);
typedef void  (*setup_fn)(void *ctl, void *anim, void *coord, void *sound,
                          void *sprite, void *mesh);
typedef void  (*state_fn)(void *ctl, int state, int layer);
typedef int   (*exists_fn)(const char *path);

static so_hook      g_ctl_hook, g_pal_hook, g_nrm_hook;
static setup_fn     g_setup;
static state_fn     g_set_state;
static exists_fn    g_file_exists;
static const char  *g_file[3];
static uint32_t     g_file_len[3];
static sdr_pool   **g_pool[3];
static sdr_pool   **g_ctl_pool;

#define SDR_NAME_MAX 255

#define SDR_ROWS    512
#define SDR_KEY_MAX 111

enum { PAL_UNKNOWN = 0, PAL_KNOWN = 1 };
enum { NRM_UNKNOWN = 0, NRM_ABSENT = 1, NRM_PRESENT = 2 };
enum { SND_UNKNOWN = 0, SND_ABSENT = 1 };

typedef struct {
	uint32_t hash;
	uint16_t klen;
	uint8_t  pal_state;
	uint8_t  nrm_state;
	uint8_t  snd_state;
	int32_t  pal_val;
	char     key[SDR_KEY_MAX + 1];
} sdr_row;

static sdr_row        g_rows[SDR_ROWS];
static uint32_t       g_used;
static lazy_lwmutex_t g_lock = LAZY_LWMUTEX_INITIALIZER;

#define ARM_ON() (1)

#define SDR_COUNT(c) ((void)0)

static uint32_t sdr_hash(const char *p, uint32_t n)
{
	uint32_t h = 2166136261u;
	for (uint32_t i = 0; i < n; i++) {
		h ^= (uint8_t)p[i];
		h *= 16777619u;
	}
	return h;
}

static sdr_row *sdr_slot(const char *k, uint32_t klen, uint32_t h)
{
	for (uint32_t i = 0; i < SDR_ROWS; i++) {
		sdr_row *row = &g_rows[(h + i) & (SDR_ROWS - 1)];
		if (row->klen == 0)
			return row;
		if (row->hash == h && row->klen == klen &&
		    memcmp(row->key, k, klen) == 0)
			return row;
	}
	return NULL;
}

static bool sdr_claim(sdr_row *row, const char *k, uint32_t klen, uint32_t h)
{
	if (row->klen != 0)
		return true;
	if (g_used >= (SDR_ROWS * 3) / 4) {
		return false;
	}
	row->hash = h;
	memcpy(row->key, k, klen);
	row->key[klen] = '\0';
	row->klen = (uint16_t)klen;
	g_used++;
	return true;
}

static uint8_t *sdr_find(const sdr_pool *p, const char *name, uint32_t len)
{
	uint32_t n = p->used;
	uint8_t **items = p->items;
	for (uint32_t i = 0; i < n; i++) {
		uint8_t *ref = items[i];
		const char *s = REF_NAME(ref);
		if (((const uint32_t *)s)[-3] == len && memcmp(s, name, len) == 0)
			return ref;
	}
	return NULL;
}

static bool sdr_sound_absent(const char *name, uint32_t len)
{
	if (len > SDR_KEY_MAX)
		return false;
	bool absent = false;
	uint32_t h = sdr_hash(name, len);
	if (lazy_lwmutex_lock(&g_lock, "sdrec_lock")) {
		sdr_row *row = sdr_slot(name, len, h);
		absent = row && row->klen && row->snd_state == SND_ABSENT;
		lazy_lwmutex_unlock(&g_lock);
	}
	return absent;
}

static void sdr_learn_sound(const char *name, uint32_t len)
{
	if (len > SDR_KEY_MAX)
		return;
	const sdr_pool *p = *g_pool[2];
	if (!p || sdr_find(p, name, len) || g_file_exists(name))
		return;
	uint32_t h = sdr_hash(name, len);
	if (lazy_lwmutex_lock(&g_lock, "sdrec_lock")) {
		sdr_row *row = sdr_slot(name, len, h);
		if (row && sdr_claim(row, name, len, h)) {
			row->snd_state = SND_ABSENT;
		} else if (!row) {
		}
		lazy_lwmutex_unlock(&g_lock);
	}
}

static void *sdr_create_controller(const char *dir, void *sprite, void *mesh)
{
	uprof_hspan_t span;
	void *ctl;
	char name[3][SDR_NAME_MAX + 1];
	uint32_t len[3];
	uint8_t *ref[3];
	bool learn_sound = false;

	if (!ARM_ON() || !dir) {
		goto stock;
	}

	uint32_t dlen = (uint32_t)strlen(dir);
	for (int k = 0; k < 3; k++) {
		len[k] = dlen + g_file_len[k];
		if (len[k] > SDR_NAME_MAX) {
			goto stock;
		}
		memcpy(name[k], dir, dlen);
		memcpy(name[k] + dlen, g_file[k], g_file_len[k] + 1);
	}
	for (int k = 0; k < 3; k++) {
		const sdr_pool *p = *g_pool[k];
		ref[k] = p ? sdr_find(p, name[k], len[k]) : NULL;
		if (ref[k])
			continue;
		if (k == 2 && sdr_sound_absent(name[2], len[2]))
			continue;
		learn_sound = (k == 2);
		goto stock;
	}

	sdr_pool *cp = *g_ctl_pool;
	if (!cp || cp->cap - 1u < cp->used) {
		goto stock;
	}

	for (int k = 0; k < 3; k++)
		if (ref[k])
			REF_COUNT(ref[k]) += 1;
	ctl = cp->items[cp->used];
	cp->used++;
	g_setup(ctl, REF_PAYLOAD(ref[0]), REF_PAYLOAD(ref[1]),
	        ref[2] ? REF_PAYLOAD(ref[2]) : NULL, sprite, mesh);
	g_set_state(ctl, 0, 0);
	if (!ref[2])
		SDR_COUNT(g_ctl_nosnd);
	return ctl;

stock:
	ctl = SO_CONTINUE(void *, g_ctl_hook, dir, sprite, mesh);
	if (ARM_ON() && dir)
		SDR_COUNT(g_ctl_stock);
	if (learn_sound)
		sdr_learn_sound(name[2], len[2]);
	return ctl;
}

static const char *sdr_str(const void *s, uint32_t *len_out, uint32_t cap)
{
	if (!s)
		return NULL;
	const char *d = *(const char *const *)s;
	if (!d)
		return NULL;
	uint32_t len = ((const uint32_t *)d)[-3];
	if (len == 0 || len > cap)
		return NULL;
	*len_out = len;
	return d;
}

static uint64_t sdr_has_palette(const void *path)
{
	uprof_hspan_t span;
	uint64_t r;

	if (!ARM_ON()) {
		r = SO_CONTINUE(uint64_t, g_pal_hook, path);
		goto out;
	}

	uint32_t klen = 0;
	const char *k = sdr_str(path, &klen, SDR_KEY_MAX);
	if (!k) {
		r = SO_CONTINUE(uint64_t, g_pal_hook, path);
		goto out;
	}
	uint32_t h = sdr_hash(k, klen);

	bool hit = false;
	int32_t val = 0;
	if (lazy_lwmutex_lock(&g_lock, "sdrec_lock")) {
		sdr_row *row = sdr_slot(k, klen, h);
		if (row && row->klen && row->pal_state == PAL_KNOWN) {
			val = row->pal_val;
			hit = true;
		}
		lazy_lwmutex_unlock(&g_lock);
	}
	if (hit) {
		r = ((uint64_t)(uintptr_t)path << 32) | (uint32_t)val;
		goto out;
	}

	r = SO_CONTINUE(uint64_t, g_pal_hook, path);
	if (lazy_lwmutex_lock(&g_lock, "sdrec_lock")) {
		sdr_row *row = sdr_slot(k, klen, h);
		if (row && sdr_claim(row, k, klen, h)) {
			row->pal_val = (int32_t)(uint32_t)r;
			row->pal_state = PAL_KNOWN;
		} else if (!row) {
		}
		lazy_lwmutex_unlock(&g_lock);
	}

out:
	return r;
}

static char *sdr_normal_name(const char *path)
{
	char *r;

	if (!ARM_ON() || !path) {
		r = SO_CONTINUE(char *, g_nrm_hook, path);
		goto out;
	}

	uint32_t klen = (uint32_t)strlen(path);
	if (klen <= 10 || klen > SDR_KEY_MAX) {
		r = SO_CONTINUE(char *, g_nrm_hook, path);
		goto out;
	}
	uint32_t h = sdr_hash(path, klen);

	uint8_t state = NRM_UNKNOWN;
	if (lazy_lwmutex_lock(&g_lock, "sdrec_lock")) {
		sdr_row *row = sdr_slot(path, klen, h);
		if (row && row->klen)
			state = row->nrm_state;
		lazy_lwmutex_unlock(&g_lock);
	}
	if (state == NRM_ABSENT) {
		r = NULL;
		goto out;
	}

	r = SO_CONTINUE(char *, g_nrm_hook, path);
	if (state == NRM_PRESENT) {
		goto out;
	}
	if (lazy_lwmutex_lock(&g_lock, "sdrec_lock")) {
		sdr_row *row = sdr_slot(path, klen, h);
		if (row && sdr_claim(row, path, klen, h))
			row->nrm_state = r ? NRM_PRESENT : NRM_ABSENT;
		else if (!row)
			SDR_COUNT(g_table_full);
		lazy_lwmutex_unlock(&g_lock);
	}

out:
	return r;
}

void sprite_dir_record_install(void)
{
	uintptr_t ctl   = so_symbol(&so_mod, CTL_SYM);
	uintptr_t pal   = so_symbol(&so_mod, PAL_SYM);
	uintptr_t nrm   = so_symbol(&so_mod, NRM_SYM);
	uintptr_t setup = so_symbol(&so_mod, SETUP_SYM);
	uintptr_t state = so_symbol(&so_mod, STATE_SYM);
	uintptr_t cpool = so_symbol(&so_mod, CTLPOOL_SYM);
	uintptr_t exist = so_symbol(&so_mod, EXISTS_SYM);
	bool ok = ctl && pal && nrm && setup && state && cpool && exist;

	uintptr_t file[3], pool[3];
	for (int k = 0; k < 3; k++) {
		file[k] = so_symbol(&so_mod, k_file_sym[k]);
		pool[k] = so_symbol(&so_mod, k_pool_sym[k]);
		ok = ok && file[k] && pool[k];
	}
	if (!ok) {
		l_error("sdrec: a symbol is missing - NOT installed, the sprite "
		        "build's fileExists chains run stock");
		return;
	}

	for (int k = 0; k < 3; k++) {
		g_file[k] = *(const char *const *)file[k];
		if (!g_file[k]) {
			l_error("sdrec: %s is NULL - NOT installed", k_file_sym[k]);
			return;
		}
		g_file_len[k] = (uint32_t)strlen(g_file[k]);
		g_pool[k] = (sdr_pool **)pool[k];
	}
	g_ctl_pool  = (sdr_pool **)cpool;
	g_setup     = (setup_fn)setup;
	g_set_state = (state_fn)state;
	g_file_exists = (exists_fn)exist;

	g_ctl_hook = hook_addr(ctl, (uintptr_t)&sdr_create_controller);
	g_pal_hook = hook_addr(pal, (uintptr_t)&sdr_has_palette);
	g_nrm_hook = hook_addr(nrm, (uintptr_t)&sdr_normal_name);
}
