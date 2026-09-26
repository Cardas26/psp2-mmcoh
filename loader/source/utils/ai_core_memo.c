#include "utils/ai_core_memo.h"

#include <stdint.h>
#include <psp2/kernel/processmgr.h>
#include <so_util/so_util.h>

#include "utils/logger.h"

extern so_module so_mod;

#define CALC_SYM  "_ZN2AI10calcScoresEiP9GameBoardii"
#define CORES_SYM "_ZN2AI10powerCoresEP9GameBoardb"
#define HIGH_SYM  "_ZN2AI13powersTooHighEP9GameBoard"

#define VERIFY_EVERY 16u

static so_hook g_calc_hook, g_cores_hook, g_high_hook;
static int     g_installed;

static uint32_t g_open;
static void    *g_board;
static uint8_t  g_have_cores[2], g_have_high;
static int      g_cores[2], g_high;

static void forget(void *board) {
	g_board = board;
	g_have_cores[0] = g_have_cores[1] = g_have_high = 0;
}

static int calc_c(void *self, int side, void *board, int a3, int a4) {
	g_open++;
	forget(board);
	(void)SO_CONTINUE(int, g_calc_hook, self, side, board, a3, a4);
	g_open--;
	forget(0);
	return 0;
}

static int cores_c(void *self, void *board, uint32_t own) {
	int k = (own & 0xff) ? 1 : 0;
	if (!g_open) {
		return SO_CONTINUE(int, g_cores_hook, self, board, own);
	}
	if (board != g_board)
		forget(board);
	if (!g_have_cores[k]) {
		g_cores[k] = SO_CONTINUE(int, g_cores_hook, self, board, own);
		g_have_cores[k] = 1;
		return g_cores[k];
	}
	return g_cores[k];
}

static int high_c(void *self, void *board) {
	if (!g_open) {
		return SO_CONTINUE(int, g_high_hook, self, board);
	}
	if (board != g_board)
		forget(board);
	if (!g_have_high) {
		g_high = SO_CONTINUE(int, g_high_hook, self, board);
		g_have_high = 1;
		return g_high;
	}
	return g_high;
}

void ai_core_memo_install(void) {
	uintptr_t calc  = so_symbol(&so_mod, CALC_SYM);
	uintptr_t cores = so_symbol(&so_mod, CORES_SYM);
	uintptr_t high  = so_symbol(&so_mod, HIGH_SYM);
	if (!calc || !cores || !high) {
		l_error("ai-memo: a symbol is missing (calc=%p cores=%p high=%p) - NOT "
		        "installed, the AI runs stock", (void *)calc, (void *)cores, (void *)high);
		return;
	}
	g_calc_hook  = hook_addr(calc,  (uintptr_t)&calc_c);
	g_cores_hook = hook_addr(cores, (uintptr_t)&cores_c);
	g_high_hook  = hook_addr(high,  (uintptr_t)&high_c);
	g_installed = 1;
}
