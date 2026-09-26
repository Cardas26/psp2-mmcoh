#include "utils/scene_entity_index.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <psp2/kernel/processmgr.h>
#include <so_util/so_util.h>

#include "utils/lazy_lwmutex.h"
#include "utils/logger.h"

extern so_module so_mod;

#define REMOVE_SYM   "_ZN5moFlo4Core6CScene12RemoveEntityERKN5boost10shared_ptrINS0_7CEntityEEE"
#define SETSCENE_SYM "_ZN5moFlo4Core7CEntity14SetOwningSceneEPNS0_6CSceneE"
#define SCDTOR_SYM   "_ZN5boost6detail12shared_countD1Ev"

typedef struct { void *px; void *pn; } sei_sp;
typedef struct { uint32_t vptr; sei_sp *begin; sei_sp *end; sei_sp *cap; } sei_scene_obj;

typedef void (*setscene_fn)(void *entity, void *scene);
typedef void (*scdtor_fn)(void *pn_field);
static setscene_fn g_set_scene;
static scdtor_fn   g_sc_dtor;

#define SEI_SLOTS  4096u
#define SEI_NPOS   3
#define SEI_TOMB   ((void *)1)
typedef struct {
	void    *scene;
	void    *ent;
	uint16_t npos;
	uint16_t pos[SEI_NPOS];
} sei_slot;

#define SEI_SCENES 16
typedef struct {
	void    *scene;
	uint32_t known;
	bool     fallback;
} sei_scene;

static sei_slot  g_slot[SEI_SLOTS];
static uint32_t  g_live, g_tomb;
static sei_scene g_scene[SEI_SCENES];
static lazy_lwmutex_t g_lock = LAZY_LWMUTEX_INITIALIZER;
static bool g_broken;

#define ARM_ON() (1)

#define SEI_COUNT(c)    ((void)0)
#define SEI_NOW()       0

static inline uint32_t sei_hash(const void *scene, const void *ent) {
	uint32_t h = (uint32_t)(uintptr_t)ent * 0x9E3779B1u;
	h ^= (uint32_t)(uintptr_t)scene * 0x85EBCA77u;
	return (h ^ (h >> 15)) & (SEI_SLOTS - 1);
}

static void sei_clear(void) {
	memset(g_slot, 0, sizeof(g_slot));
	g_live = g_tomb = 0;
	for (int i = 0; i < SEI_SCENES; i++)
		g_scene[i].known = 0;
}

static int sei_find(const void *scene, const void *ent) {
	uint32_t h = sei_hash(scene, ent);
	for (uint32_t n = 0; n < SEI_SLOTS; n++, h = (h + 1) & (SEI_SLOTS - 1)) {
		sei_slot *s = &g_slot[h];
		if (!s->ent)
			return -1;
		if (s->ent == ent && s->scene == scene)
			return (int)h;
	}
	return -1;
}

static bool sei_add(void *scene, void *ent, uint32_t pos) {
	int i = sei_find(scene, ent);
	if (i >= 0) {
		sei_slot *s = &g_slot[i];
		if (s->npos >= SEI_NPOS)
			return false;
		if (s->npos == 1)
			SEI_COUNT(g_dup_keys);
		s->pos[s->npos++] = (uint16_t)pos;
		return true;
	}
	if (g_live + g_tomb >= (SEI_SLOTS * 3u) / 4u)
		return false;
	uint32_t h = sei_hash(scene, ent);
	for (uint32_t n = 0; n < SEI_SLOTS; n++, h = (h + 1) & (SEI_SLOTS - 1)) {
		sei_slot *s = &g_slot[h];
		if (!s->ent || s->ent == SEI_TOMB) {
			if (s->ent == SEI_TOMB)
				g_tomb--;
			s->scene = scene;
			s->ent = ent;
			s->npos = 1;
			s->pos[0] = (uint16_t)pos;
			g_live++;
			return true;
		}
	}
	return false;
}

static void sei_drop_pos(sei_slot *s, uint32_t pos) {
	for (int k = 0; k < s->npos; k++) {
		if (s->pos[k] == pos) {
			s->pos[k] = s->pos[--s->npos];
			break;
		}
	}
	if (!s->npos) {
		s->ent = SEI_TOMB;
		s->scene = NULL;
		g_live--;
		g_tomb++;
	}
}

static sei_scene *sei_scene_rec(void *scene) {
	sei_scene *free_rec = NULL;
	for (int i = 0; i < SEI_SCENES; i++) {
		if (g_scene[i].scene == scene)
			return &g_scene[i];
		if (!g_scene[i].scene && !free_rec)
			free_rec = &g_scene[i];
	}
	if (free_rec) {
		free_rec->scene = scene;
		free_rec->known = 0;
		free_rec->fallback = false;
	}
	return free_rec;
}

static void sei_mutate(sei_scene_obj *sc, sei_sp *f) {
	g_set_scene(f->px, NULL);
	sei_sp *last = sc->end - 1;
	sei_sp tmp = *f;
	*f = *last;
	*last = tmp;
	sc->end = last;
	g_sc_dtor(&last->pn);
}

static sei_sp *sei_scan(sei_scene_obj *sc, void *e, uint32_t *scanned) {
	sei_sp *b = sc->begin, *end = sc->end, *f = b;
	for (; f != end; f++)
		if (f->px == e)
			break;
	*scanned = (uint32_t)(f - b) + (f != end ? 1u : 0u);
	return f != end ? f : NULL;
}

static sei_sp *sei_locate(sei_scene_obj *sc, void *e) {
	sei_sp *b = sc->begin;
	uint32_t size = (uint32_t)(sc->end - b);
	sei_scene *rec = sei_scene_rec(sc);
	if (!rec || rec->fallback || size > 0xFFFFu) {
		if (rec) rec->fallback = true;
		return (sei_sp *)SEI_TOMB;
	}
	if (rec->known > size)
		sei_clear();
	if (g_live + g_tomb + (size - rec->known) >= (SEI_SLOTS * 3u) / 4u)
		sei_clear();
	for (uint32_t i = rec->known; i < size; i++) {
		if (!sei_add(sc, b[i].px, i)) {
			rec->fallback = true;
			return (sei_sp *)SEI_TOMB;
		}
	}
	rec->known = size;

	int ki = sei_find(sc, e);
	if (ki < 0)
		return NULL;
	sei_slot *s = &g_slot[ki];
	uint32_t p = s->pos[0];
	for (int k = 1; k < s->npos; k++)
		if (s->pos[k] < p) p = s->pos[k];
	uint32_t last = size - 1;
	void *elast = b[last].px;
	int li = (last != p) ? sei_find(sc, elast) : ki;
	bool ok = b[p].px == e && li >= 0;
	if (ok && last != p) {
		ok = false;
		for (int k = 0; k < g_slot[li].npos; k++)
			if (g_slot[li].pos[k] == last) ok = true;
	}
	if (!ok) {
		sei_clear();
		return (sei_sp *)SEI_TOMB;
	}
	sei_drop_pos(s, p);
	if (last != p) {
		sei_slot *ls = &g_slot[li];
		for (int k = 0; k < ls->npos; k++)
			if (ls->pos[k] == last) { ls->pos[k] = (uint16_t)p; break; }
	}
	rec->known = size - 1;
	if (g_tomb > SEI_SLOTS / 4u)
		sei_clear();
	return &b[p];
}

static void sei_remove_entity(sei_scene_obj *sc, const sei_sp *arg) {
	void *e = arg->px;
	uint64_t t0 = SEI_NOW();
	uint32_t scanned = 0;
	sei_sp *f;

	if (!ARM_ON() || g_broken) {
		f = sei_scan(sc, e, &scanned);
		if (ARM_ON()) SEI_COUNT(g_fallback_calls);
	} else if (!lazy_lwmutex_lock(&g_lock, "sceneidx_lock")) {
		g_broken = true;
		f = sei_scan(sc, e, &scanned);
	} else {
		f = sei_locate(sc, e);
		lazy_lwmutex_unlock(&g_lock);
		if (f == (sei_sp *)SEI_TOMB) {
			f = sei_scan(sc, e, &scanned);
		}
	}
	if (f) {
		sei_mutate(sc, f);
	} else {
	}
	(void)scanned;
	(void)t0;
}

void scene_entity_index_install(void) {
	uintptr_t rm = so_symbol(&so_mod, REMOVE_SYM);
	g_set_scene = (setscene_fn)so_symbol(&so_mod, SETSCENE_SYM);
	g_sc_dtor   = (scdtor_fn)so_symbol(&so_mod, SCDTOR_SYM);
	if (!rm || !g_set_scene || !g_sc_dtor) {
		l_error("sceneidx: a symbol is missing - NOT installed, "
		        "CScene::RemoveEntity scans stock");
		return;
	}
	hook_addr(rm, (uintptr_t)&sei_remove_entity);
}
