#include "utils/cheatsheet_grid_gate.h"

#include <stdint.h>
#include <so_util/so_util.h>

#include "utils/logger.h"

extern so_module so_mod;

#define GRID_SYM   "_ZN9GameBoard27updateCheatSheetGridSpritesEv"
#define ENGINE_SYM "_ZN6Engine14getInstancePtrEv"

#define BOARD_FLAG_OFF     0x81c
#define BOARD_SPRITE0_OFF  0x820
#define ENGINE_OBJ_OFF     0xbc
#define ENGINE_FLAG_OFF    0x26b

typedef void *(*engine_instance_fn)(void);
static engine_instance_fn g_engine_instance;
static so_hook g_hook;

#define ENTRIES 8
typedef struct {
	const void *board;
	const void *sprite0;
	uint8_t hidden_settled;
} gate_entry;
static gate_entry g_entries[ENTRIES];
static uint32_t g_next_entry;

#define GATE_ON() (1)

#define GATE_COUNT(c)  ((void)0)

static gate_entry *entry_for(const void *board, const void *sprite0) {
	for (int i = 0; i < ENTRIES; i++)
		if (g_entries[i].board == board && g_entries[i].sprite0 == sprite0)
			return &g_entries[i];
	gate_entry *e = &g_entries[g_next_entry];
	g_next_entry = (g_next_entry + 1u) % ENTRIES;
	if (e->board) GATE_COUNT(g_evicted);
	e->board = board;
	e->sprite0 = sprite0;
	e->hidden_settled = 0;
	return e;
}

static int engine_flag(void) {
	const uint8_t *engine = (const uint8_t *)g_engine_instance();
	if (!engine) return 1;
	const uint8_t *obj = *(const uint8_t *const *)(engine + ENGINE_OBJ_OFF);
	if (!obj) return 1;
	return obj[ENGINE_FLAG_OFF] != 0;
}

static int hook_updateCheatSheetGridSprites(void *self) {

	const uint8_t *b = (const uint8_t *)self;
	int flag = b[BOARD_FLAG_OFF] != 0;
	int eng = engine_flag();
	int hidden = !flag && !eng;

	const void *sprite0 = *(const void *const *)(b + BOARD_SPRITE0_OFF);
	gate_entry *e = entry_for(self, sprite0);

	if (GATE_ON() && hidden && e->hidden_settled) {
		return 0;
	}

	int r = SO_CONTINUE(int, g_hook, self);
	e->hidden_settled = (uint8_t)hidden;
	return r;
}

void cheatsheet_grid_gate_install(void) {
	uintptr_t grid = so_symbol(&so_mod, GRID_SYM);
	uintptr_t engine = so_symbol(&so_mod, ENGINE_SYM);
	if (!grid || !engine) {
		l_error("cheatgrid-gate: %s not found - NOT installed, the grid "
		        "sprites update stock", !grid ? GRID_SYM : ENGINE_SYM);
		return;
	}
	g_engine_instance = (engine_instance_fn)engine;
	g_hook = hook_addr(grid, (uintptr_t)&hook_updateCheatSheetGridSprites);
}
