#include "utils/cheatsheet_lazy.h"

#include <stdint.h>
#include <so_util/so_util.h>

#include "utils/logger.h"

extern so_module so_mod;

#define CREATE_SYM   "_ZN6Entity23createCheatSheetSpritesEv"
#define SHOW_SYM     "_ZN6Entity14showCheatSheetEb"
#define ICON_DEL_SYM "_ZN6Entity20deinitCheatSheetIconEv"
#define NUM_DEL_SYM  "_ZN6Entity22deinitCheatSheetNumberEv"
#define UPDATE_SYM   "_ZN6Entity16updateCheatSheetEv"
#define SPR_VIS_SYM  "_ZN9NewSprite10setVisibleEb"
#define SPR_DEL_SYM  "_ZN9NewSprite6deinitEv"
#define TXT_VIS_SYM  "_ZN10BitmapText10setVisibleEb"
#define TXT_DEL_SYM  "_ZN10BitmapText7destroyEPS_"
#define ENGINE_SYM   "_ZN6Engine14getInstancePtrEv"

#define E_CS_ICON    0xb8
#define E_CS_NUMBER  0xbc
#define E_CS_SHOWN   0xc0
#define ENGINE_PS    0xbc
#define PS_CS_FLAG   0xe4

typedef void *(*engine_instance_fn)(void);
typedef void  (*obj_fn)(void *);
typedef void  (*obj_bool_fn)(void *, int);

static engine_instance_fn g_engine_instance;
static obj_fn      g_update_cs, g_spr_deinit, g_txt_destroy;
static obj_bool_fn g_spr_visible, g_txt_visible;
static so_hook     g_create_hook;

#define SET_BITS 10
#define SET_SIZE (1u << SET_BITS)
#define SET_MAX  (SET_SIZE * 3u / 4u)
typedef struct {
	void   *e;
	uint8_t no_icon;
} pend_slot;
static pend_slot g_set[SET_SIZE];
static uint32_t  g_set_n;

#define LAZY_ON() (1)

static inline void *rdp(const void *p, uint32_t off)
{
	return *(void *const volatile *)((const uint8_t *)p + off);
}
static inline void wrp(void *p, uint32_t off, void *v)
{
	*(void *volatile *)((uint8_t *)p + off) = v;
}

static inline uint32_t set_home(const void *e)
{
	return ((uint32_t)(uintptr_t)e * 2654435761u) >> (32 - SET_BITS);
}

static pend_slot *set_find(const void *e)
{
	for (uint32_t i = set_home(e);; i = (i + 1) & (SET_SIZE - 1)) {
		if (g_set[i].e == e) return &g_set[i];
		if (!g_set[i].e) return NULL;
	}
}

static pend_slot *set_insert(void *e)
{
	uint32_t i = set_home(e);
	for (;; i = (i + 1) & (SET_SIZE - 1)) {
		if (g_set[i].e == e) return &g_set[i];
		if (!g_set[i].e) break;
	}
	if (g_set_n >= SET_MAX) return NULL;
	g_set[i].e = e;
	g_set[i].no_icon = 0;
	g_set_n++;
	return &g_set[i];
}

static void set_remove(pend_slot *s)
{
	uint32_t i = (uint32_t)(s - g_set);
	g_set[i].e = NULL;
	g_set_n--;
	for (uint32_t j = (i + 1) & (SET_SIZE - 1); g_set[j].e; j = (j + 1) & (SET_SIZE - 1)) {
		uint32_t h = set_home(g_set[j].e);
		int stays = (i <= j) ? (i < h && h <= j) : (i < h || h <= j);
		if (stays) continue;
		g_set[i] = g_set[j];
		g_set[j].e = NULL;
		i = j;
	}
}

static int sheet_enabled(void)
{
	const uint8_t *engine = (const uint8_t *)g_engine_instance();
	if (!engine) return 1;
	const uint8_t *ps = (const uint8_t *)rdp(engine, ENGINE_PS);
	if (!ps) return 1;
	return ps[PS_CS_FLAG] != 0;
}

static int hook_create(void *self)
{
	uint8_t *e = (uint8_t *)self;
	if (LAZY_ON() && !rdp(e, E_CS_ICON) && !rdp(e, E_CS_NUMBER) &&
	    !e[E_CS_SHOWN] && !sheet_enabled()) {
		pend_slot *s = set_insert(self);
		if (s) {
			s->no_icon = 0;
			return 0;
		}
	}
	return SO_CONTINUE(int, g_create_hook, self);
}

static int hook_show(void *self, int show)
{
	uint8_t *e = (uint8_t *)self;
	if (show && !rdp(e, E_CS_ICON) && !rdp(e, E_CS_NUMBER)) {
		pend_slot *s = set_find(self);
		if (s) {
			int no_icon = s->no_icon;
			set_remove(s);
			SO_CONTINUE(int, g_create_hook, self);
			if (no_icon) {
				void *icon = rdp(e, E_CS_ICON);
				if (icon) {
					g_spr_deinit(icon);
					wrp(e, E_CS_ICON, NULL);
				}
			}
		}
	}
	e[E_CS_SHOWN] = (uint8_t)show;
	void *icon = rdp(e, E_CS_ICON);
	if (icon) g_spr_visible(icon, show);
	void *number = rdp(e, E_CS_NUMBER);
	if (number) g_txt_visible(number, show);
	if (show) g_update_cs(self);
	return 0;
}

static int hook_icon_del(void *self)
{
	uint8_t *e = (uint8_t *)self;
	void *icon = rdp(e, E_CS_ICON);
	if (icon) {
		g_spr_deinit(icon);
		wrp(e, E_CS_ICON, NULL);
		return 0;
	}
	if (!rdp(e, E_CS_NUMBER) && g_set_n) {
		pend_slot *s = set_find(self);
		if (s) s->no_icon = 1;
	}
	return 0;
}

static int hook_number_del(void *self)
{
	uint8_t *e = (uint8_t *)self;
	void *number = rdp(e, E_CS_NUMBER);
	if (number) {
		g_txt_destroy(number);
		wrp(e, E_CS_NUMBER, NULL);
		return 0;
	}
	if (g_set_n) {
		pend_slot *s = set_find(self);
		if (s) {
			set_remove(s);
		}
	}
	return 0;
}

void cheatsheet_lazy_install(void)
{
	static const char *const syms[] = {
		CREATE_SYM, SHOW_SYM, ICON_DEL_SYM, NUM_DEL_SYM, UPDATE_SYM,
		SPR_VIS_SYM, SPR_DEL_SYM, TXT_VIS_SYM, TXT_DEL_SYM, ENGINE_SYM,
	};
	uintptr_t a[sizeof(syms) / sizeof(syms[0])];
	for (unsigned i = 0; i < sizeof(syms) / sizeof(syms[0]); i++) {
		a[i] = so_symbol(&so_mod, syms[i]);
		if (!a[i]) {
			l_error("cheat-lazy: %s not found - NOT installed, every cheat "
			        "sheet is built stock", syms[i]);
			return;
		}
	}
	g_update_cs       = (obj_fn)a[4];
	g_spr_visible     = (obj_bool_fn)a[5];
	g_spr_deinit      = (obj_fn)a[6];
	g_txt_visible     = (obj_bool_fn)a[7];
	g_txt_destroy     = (obj_fn)a[8];
	g_engine_instance = (engine_instance_fn)a[9];
	g_create_hook = hook_addr(a[0], (uintptr_t)&hook_create);
	hook_addr(a[1], (uintptr_t)&hook_show);
	hook_addr(a[2], (uintptr_t)&hook_icon_del);
	hook_addr(a[3], (uintptr_t)&hook_number_del);
}
