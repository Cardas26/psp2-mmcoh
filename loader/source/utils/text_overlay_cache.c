#include "utils/text_overlay_cache.h"

#include <stdint.h>
#include <so_util/so_util.h>

#include "utils/logger.h"

extern so_module so_mod;

#define TEXTOVL_UPDATE_SYM "_ZN17TextOverlayScreen6updateEf"
#define TEXTOVL_ALPHA_SYM  "_ZN17TextOverlayScreen7m_alphaE"
#define TEXTOVL_TEXT_SYM   "_ZN17TextOverlayScreen6m_textE"
#define TEXTOVL_X_SYM      "_ZN17TextOverlayScreen3m_xE"
#define TEXTOVL_Y_SYM      "_ZN17TextOverlayScreen3m_yE"
#define TEXTOVL_CTEXT_SYM  "_ZN17TextOverlayScreen12m_centerTextE"
#define TEXTOVL_CBASE_SYM  "_ZN17TextOverlayScreen16m_centerBaselineE"

#define TEXTOVL_GUARD_ANCHOR_SYM "_ZN8Subtitle11s_subtitlesE"
#define TEXTOVL_GUARD_OFFSET     4

#define TEXTOVL_QUADS_OFF  0x14
#define TEXTOVL_QUAD_COUNT 256
#define TEXTOVL_FONT_OFF   0x41c

#define WSTR_LEN(data) ((data)[-3])

#define TEXTOVL_MAX_CHARS 1024

typedef struct {
	void *self;
	uint64_t hash;
} textovl_row;

#define TEXTOVL_ROWS 8
static textovl_row g_rows[TEXTOVL_ROWS];

static so_hook g_hook;
static const uint32_t *volatile *g_text;
static const uint32_t *g_alpha;
static const int32_t *g_x, *g_y;
static const uint8_t *g_center_text, *g_center_baseline;
static void *const *g_guard;

#define GATE_ON() (1)

#define CACHE_COUNT(c) ((void)0)

static inline uint64_t fnv(uint64_t h, uint32_t w) {
	h ^= w;
	return h * 1099511628211ull;
}

static uint64_t textovl_hash(void *self) {
	const uint32_t *text = (const uint32_t *)*g_text;
	uint32_t len = text ? WSTR_LEN(text) : 0;
	if (text && len > TEXTOVL_MAX_CHARS) {
		return 0;
	}

	uint64_t h = 14695981039346656037ull;
	h = fnv(h, (uint32_t)(uintptr_t)self);
	h = fnv(h, (uint32_t)(uintptr_t)*g_guard);
	h = fnv(h, *g_alpha);
	h = fnv(h, (uint32_t)*g_x);
	h = fnv(h, (uint32_t)*g_y);
	h = fnv(h, *g_center_text);
	h = fnv(h, *g_center_baseline);
	h = fnv(h, *(const uint32_t *)((const uint8_t *)self + TEXTOVL_FONT_OFF));

	h = fnv(h, (uint32_t)(uintptr_t)text);
	h = fnv(h, len);
	for (uint32_t i = 0; i < len; i++)
		h = fnv(h, text[i]);

	const uint32_t *quads = (const uint32_t *)((const uint8_t *)self + TEXTOVL_QUADS_OFF);
	for (uint32_t i = 0; i < TEXTOVL_QUAD_COUNT; i++)
		h = fnv(h, quads[i]);

	return h ? h : 1;
}

static textovl_row *row_for(void *self) {
	for (int i = 0; i < TEXTOVL_ROWS; i++) {
		if (g_rows[i].self == self || !g_rows[i].self) {
			g_rows[i].self = self;
			return &g_rows[i];
		}
	}
	return NULL;
}

static void hook_TextOverlayScreen_update(void *self, uint32_t dt_bits) {

	uint64_t h = textovl_hash(self);
	textovl_row *row = row_for(self);

	if (GATE_ON() && h && row && row->hash == h) {
		return;
	}

	SO_CONTINUE(int, g_hook, self, dt_bits);

	if (row) {
		uint64_t after = textovl_hash(self);
		row->hash = after;
	}
}

void text_overlay_cache_install(void) {
	uintptr_t update_addr = so_symbol(&so_mod, TEXTOVL_UPDATE_SYM);
	uintptr_t anchor      = so_symbol(&so_mod, TEXTOVL_GUARD_ANCHOR_SYM);

	g_text            = (const uint32_t *volatile *)so_symbol(&so_mod, TEXTOVL_TEXT_SYM);
	g_alpha           = (const uint32_t *)so_symbol(&so_mod, TEXTOVL_ALPHA_SYM);
	g_x               = (const int32_t *)so_symbol(&so_mod, TEXTOVL_X_SYM);
	g_y               = (const int32_t *)so_symbol(&so_mod, TEXTOVL_Y_SYM);
	g_center_text     = (const uint8_t *)so_symbol(&so_mod, TEXTOVL_CTEXT_SYM);
	g_center_baseline = (const uint8_t *)so_symbol(&so_mod, TEXTOVL_CBASE_SYM);
	g_guard           = anchor ? (void *const *)(anchor + TEXTOVL_GUARD_OFFSET) : NULL;

	if (!update_addr || !g_text || !g_alpha || !g_x || !g_y ||
	    !g_center_text || !g_center_baseline || !g_guard) {
		l_error("textovl-cache: symbols missing (update=%p text=%p alpha=%p "
		        "x=%p y=%p ctext=%p cbase=%p guard=%p) - NOT installed, the "
		        "overlay updates stock",
		        (void *)update_addr, (void *)g_text, (void *)g_alpha,
		        (void *)g_x, (void *)g_y, (void *)g_center_text,
		        (void *)g_center_baseline, (void *)g_guard);
		g_text = NULL;
		return;
	}

	g_hook = hook_addr(update_addr, (uintptr_t)&hook_TextOverlayScreen_update);
	l_perf("[textovl-cache] TextOverlayScreen::update gated - a call whose "
	       "%u quads, font, text and five statics are unchanged skips 512 "
	       "quad writes", (unsigned)TEXTOVL_QUAD_COUNT);
}
