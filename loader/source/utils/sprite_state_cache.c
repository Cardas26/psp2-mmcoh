#include "utils/sprite_state_cache.h"

#include <stdint.h>
#include <so_util/so_util.h>

#include "utils/logger.h"
#include "utils/sprite_state.h"

extern so_module so_mod;

#define SPRITE_MANAGER_UPDATE_SYM "_ZN16NewSpriteManager6updateEf"
#define SPRITE_ACTIVE_SIZE_SYM    "_ZN16NewSpriteManager13getActiveSizeEv"
#define SPRITE_ACTIVE_SPRITE_SYM  "_ZN16NewSpriteManager15getActiveSpriteEj"
#define NEWSPRITE_PROCESS_SYM     "_ZN9NewSprite7processEf"

typedef uint32_t (*so_active_size_fn)(void);
typedef void *   (*so_active_sprite_fn)(uint32_t index);
typedef void     (*so_process_fn)(void *sprite, uint32_t dt_bits);

static so_active_size_fn   g_active_size;
static so_active_sprite_fn g_active_sprite;
static so_process_fn       g_process;

#define TAB_BITS  10
#define TAB_SIZE  (1u << TAB_BITS)
#define TAB_PROBE 32

static struct {
	void *key;
	const void *mesh;
	uint32_t words[SPRITE_STATE_WORDS];
} g_tab[TAB_SIZE];

#define GATE_ON() (1)

#define CACHE_COUNT(c) ((void)0)

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

static void hook_NewSpriteManager_update(void *self, uint32_t dt_bits) {
	(void)self;

	for (uint32_t i = 0; i < g_active_size(); i++) {
		void *s = g_active_sprite(i);
		if (!s) continue;

		const void *mesh = (const void *)SPRITE_RD32(s, F_NODE);
		uint8_t was_dirty = SPRITE_RD8(s, F_DIRTY);
		uint8_t always = SPRITE_RD8(s, F_ALWAYS);
		uint32_t row = 0;
		int have_row = tab_index(s, &row);

		uint8_t saved = 0;
		if (GATE_ON() && mesh && !always && was_dirty && have_row) {
			if (g_tab[row].mesh != mesh) {
			} else if (sprite_state_unchanged(s, g_tab[row].words)) {
				saved = was_dirty;
				SPRITE_WR8(s, F_DIRTY, 0);
			}
		}

		g_process(s, dt_bits);

		if (saved) {
			SPRITE_WR8(s, F_DIRTY, saved);
		} else if (have_row && mesh) {
			sprite_state_snap(s, g_tab[row].words);
			g_tab[row].mesh = (const void *)SPRITE_RD32(s, F_NODE);
			if (was_dirty || always) CACHE_COUNT(g_rebuilt);
		}

	}

}

void sprite_state_cache_install(void) {
	uintptr_t update_addr = so_symbol(&so_mod, SPRITE_MANAGER_UPDATE_SYM);
	g_active_size   = (so_active_size_fn)so_symbol(&so_mod, SPRITE_ACTIVE_SIZE_SYM);
	g_active_sprite = (so_active_sprite_fn)so_symbol(&so_mod, SPRITE_ACTIVE_SPRITE_SYM);
	g_process       = (so_process_fn)so_symbol(&so_mod, NEWSPRITE_PROCESS_SYM);

	if (!update_addr || !g_active_size || !g_active_sprite || !g_process) {
		l_error("state_cache: symbols missing (update=%p size=%p sprite=%p "
		        "process=%p) - NOT installed, sprites update stock",
		        (void *)update_addr, (void *)g_active_size,
		        (void *)g_active_sprite, (void *)g_process);
		g_active_size = NULL;
		g_active_sprite = NULL;
		g_process = NULL;
		return;
	}

	hook_addr(update_addr, (uintptr_t)&hook_NewSpriteManager_update);
	l_perf("[state-cache] NewSpriteManager::update replaced - a sprite whose "
	       "%u transform words are unchanged keeps its dirty bit and skips "
	       "the rebuild", (unsigned)SPRITE_STATE_WORDS);
}
