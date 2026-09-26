#include "utils/bmptext_cache.h"

#include <stdint.h>
#include <psp2/kernel/processmgr.h>
#include <so_util/so_util.h>

#include "utils/logger.h"
#include "utils/plr_ui_diag.h"

extern so_module so_mod;

#define BMPTEXT_SETTEXT_W_SYM "_ZN10BitmapText7setTextERKSbIwSt11char_traitsIwESaIwEE"
#define BMPTEXT_SETTEXT_N_SYM "_ZN10BitmapText7setTextERKSs"

static const uint16_t k_state_off[] = {
	0x04, 0x08, 0x14,
	0x18,
	0x20, 0x24, 0x28, 0x2c,
	0x30, 0x34,
	0x38,
	0x3c,
	0x40, 0x44,
	0x54, 0x58, 0x5c,
	0x60, 0x64,
	0x68,
	0x6c,
	0x70, 0x74,
};
#define BMPTEXT_STATE_WORDS (sizeof(k_state_off) / sizeof(k_state_off[0]))

#define BMPTEXT_TEXT_OFF     0x1c
#define BMPTEXT_LETTERS_OFF  0x60
#define BMPTEXT_LETTER_SIZE  20

#define WSTR_LEN(data) (((const uint32_t *)(data))[-3])

#define BMPTEXT_MAX_CHARS   1024
#define BMPTEXT_MAX_LETTERS 1024
#define BMPTEXT_MAX_BYTES   4096

typedef struct {
	void *self;
	uint64_t hash;
	uint64_t nstate;
	uint64_t ntext;
} bmptext_row;

#define BMPTEXT_ROWS 256
static bmptext_row g_rows[BMPTEXT_ROWS];

static so_hook g_hook;
static so_hook g_narrow_hook;

#define GATE_ON() (1)

#define NARROW_ON()    (1)
#define NARROW_PHASE() ("ON ")

#define CACHE_COUNT(c)  ((void)0)

#define N_CTX     1
#define N_CTX_NOW 0u

#define N_CLOCK()      0
#define N_COUNT(c)     ((void)0)

static inline uint64_t fnv(uint64_t h, uint32_t w) {
	h ^= w;
	return h * 1099511628211ull;
}

static uint64_t bmptext_state_hash(void *self) {
	const uint8_t *base = (const uint8_t *)self;

	uint64_t h = 14695981039346656037ull;
	h = fnv(h, (uint32_t)(uintptr_t)self);
	for (unsigned i = 0; i < BMPTEXT_STATE_WORDS; i++)
		h = fnv(h, *(const uint32_t *)(base + k_state_off[i]));

	const uint32_t *begin = *(const uint32_t *const *)(base + BMPTEXT_LETTERS_OFF);
	const uint32_t *end   = *(const uint32_t *const *)(base + BMPTEXT_LETTERS_OFF + 4);
	if (begin && end >= begin) {
		uintptr_t bytes = (uintptr_t)end - (uintptr_t)begin;
		if (bytes > (uintptr_t)BMPTEXT_MAX_LETTERS * BMPTEXT_LETTER_SIZE) {
			return 0;
		}
		for (uint32_t i = 0; i < bytes / 4; i++)
			h = fnv(h, begin[i]);
	} else if (begin != end) {
		return 0;
	}

	return h ? h : 1;
}

static uint64_t bmptext_text_hash(uint64_t h, const uint32_t *text) {
	if (!h) return 0;

	uint32_t len = 0;
	if (text) {
		if ((uintptr_t)text & 3u) return 0;
		len = WSTR_LEN(text);
		if (len > BMPTEXT_MAX_CHARS) {
			return 0;
		}
	}

	h = fnv(h, len);
	for (uint32_t i = 0; i < len; i++)
		h = fnv(h, text[i]);

	return h ? h : 1;
}

static uint64_t bmptext_hash(void *self, const uint32_t *text) {
	return bmptext_text_hash(bmptext_state_hash(self), text);
}

static bmptext_row *row_for(void *self) {
	for (int i = 0; i < BMPTEXT_ROWS; i++) {
		if (g_rows[i].self == self || !g_rows[i].self) {
			g_rows[i].self = self;
			return &g_rows[i];
		}
	}
	return NULL;
}

static int hook_BitmapText_setText(void *self, const void *str) {

	const uint32_t *incoming = str ? *(const uint32_t *const *)str : NULL;
	uint64_t h = bmptext_hash(self, incoming);
	bmptext_row *row = row_for(self);

	if (GATE_ON() && h && row && row->hash == h) {
		return 0;
	}

	int ret = SO_CONTINUE(int, g_hook, self, str);

	if (row) {
		const uint32_t *held =
			*(const uint32_t *const *)((const uint8_t *)self + BMPTEXT_TEXT_OFF);
		row->hash = bmptext_hash(self, held);
	}
	return ret;
}

static uint64_t bmptext_bytes_hash(const char *s) {
	if (!s) return 0;

	uint64_t h = 14695981039346656037ull;
	for (uint32_t n = 0; s[n]; n++) {
		if (n >= BMPTEXT_MAX_BYTES) {
			return 0;
		}
		h = fnv(h, (uint8_t)s[n]);
	}
	return h ? h : 1;
}

static int hook_BitmapText_setText_narrow(void *self, const void *str) {
	uint64_t t0 = N_CLOCK();
	uint32_t ctx = N_CTX_NOW;

	const uint32_t *held =
		*(const uint32_t *const *)((const uint8_t *)self + BMPTEXT_TEXT_OFF);
	uint64_t key = bmptext_text_hash(bmptext_state_hash(self), held);
	uint64_t txt = bmptext_bytes_hash(str ? *(const char *const *)str : NULL);
	bmptext_row *row = row_for(self);

	int usable = key && txt && row;
	int hit    = usable && row->nstate == key && row->ntext == txt;
	if (!usable)                 N_COUNT(g_n_declined);
	else if (hit)                N_COUNT(g_n_hit);
	else if (row->nstate != key) N_COUNT(g_n_miss_state);
	else                         N_COUNT(g_n_miss_text);

	if (hit && NARROW_ON()) {
		return (int)(uintptr_t)self;
	}

	int ret = SO_CONTINUE(int, g_narrow_hook, self, str);

	if (row) {
		const uint32_t *now =
			*(const uint32_t *const *)((const uint8_t *)self + BMPTEXT_TEXT_OFF);
		row->nstate = bmptext_text_hash(bmptext_state_hash(self), now);
		row->ntext  = txt;
	}
	return ret;
}

void bmptext_cache_install(void) {
	uintptr_t addr = so_symbol(&so_mod, BMPTEXT_SETTEXT_W_SYM);
	if (!addr) {
		l_error("bmptext-cache: %s missing - NOT installed, text lays out stock",
		        BMPTEXT_SETTEXT_W_SYM);
		return;
	}

	g_hook = hook_addr(addr, (uintptr_t)&hook_BitmapText_setText);
	l_perf("[bmptext-cache] BitmapText::setText gated on %u state words plus "
	       "the string and the letter array",
	       (unsigned)BMPTEXT_STATE_WORDS);

	uintptr_t naddr = so_symbol(&so_mod, BMPTEXT_SETTEXT_N_SYM);
	if (!naddr) {
		l_error("bmptext-narrow: %s missing - NOT installed, every string is "
		        "decoded", BMPTEXT_SETTEXT_N_SYM);
		return;
	}
	g_narrow_hook = hook_addr(naddr, (uintptr_t)&hook_BitmapText_setText_narrow);
	l_perf("[bmptext-narrow] BitmapText::setText(std::string) gated on the same "
	       "%u state words, the wstring it holds and the incoming bytes - the "
	       "UTF-8 decode and its allocation are what a skip removes",
	       (unsigned)BMPTEXT_STATE_WORDS);
}
