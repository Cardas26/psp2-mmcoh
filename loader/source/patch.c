/*
 * Copyright (C) 2023 Volodymyr Atamanenko
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include <kubridge.h>
#include <so_util/so_util.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <vitasdk.h>

#ifdef __cplusplus
extern "C"
{
#endif
	extern so_module so_mod;
#ifdef __cplusplus
};
#endif

#define SCE_KERNEL_MEMBLOCK_TYPE_USER_RX (0x0C20D050)

#include "utils/logger.h"
#include "utils/dialog.h"
#include "utils/init.h"
#include "reimpl/sys.h"
#include "reimpl/audio.h"
#include "utils/render_split.h"
#include "utils/event_update_gate.h"
#include "utils/render_path_profile.h"
#include "utils/render_command_cache.h"
#include "utils/render_find.h"
#include "utils/render_material.h"
#include "utils/render_rbuf.h"
#include "utils/render_sprdata.h"
#include "utils/ai_board_copy.h"
#include "utils/ai_core_memo.h"
#include "utils/cheatsheet_lazy.h"
#include "utils/palette_info_memo.h"
#include "utils/sprite_dir_record.h"
#include "utils/scene_entity_index.h"
#include "utils/render_sortprep.h"
#include "utils/render_dynrend.h"
#include "utils/sprite_setter_c.h"
#include "utils/plr_ui_diag.h"
#include "utils/sprite_state_cache.h"
#include "utils/bmptext_cache.h"
#include "utils/cheatsheet_grid_gate.h"
#include "utils/transform_same_gate.h"
#include "utils/transform_cheap_push.h"
#include "utils/game_log_quiet.h"
#include "audio/music_rewrite.h"
#include "audio/studio.h"
#include "audio/save_names.h"
#include "utils/text_overlay_cache.h"
#include "utils/softfloat_vfp.h"
#include "utils/update_profile.h"
#include "utils/fast_crc32.h"
#include "utils/loading_drain.h"
#include <stdbool.h>

void __kuser_memory_barrier(void) {
	__sync_synchronize();
}

void kuser_patch(void) {
	SceKernelAllocMemBlockKernelOpt opt;
	memset(&opt, 0, sizeof(SceKernelAllocMemBlockKernelOpt));
	opt.size = sizeof(SceKernelAllocMemBlockKernelOpt);
	opt.attr = 0x1;
	opt.field_C = (SceUInt32)0x9A000000;
	if (kuKernelAllocMemBlock("atomic", SCE_KERNEL_MEMBLOCK_TYPE_USER_RX, 0x1000, &opt) < 0)
		fatal_error("Error could not allocate atomic block.");
	kuKernelMemProtect((void *)0x9A000000, (SceSize)0x1000, KU_KERNEL_PROT_EXEC | KU_KERNEL_PROT_READ | KU_KERNEL_PROT_WRITE);

	hook_addr(0x9A000FA0, (uintptr_t)__kuser_memory_barrier);
	hook_addr(0x9A000FC0, (uintptr_t)__atomic_cmpxchg);

	uint32_t patched_addr;
	for (uint32_t addr = so_mod.text_base; addr < so_mod.text_base + so_mod.text_size; addr += 4) {
		uint32_t *a = (uint32_t *)addr;
		if (*a == 0xFFFF0FC0) {
			l_debug("Patching %p -> __kuser_cmpxchg", (void *)a);
			patched_addr = 0x9A000FC0;
			kuKernelCpuUnrestrictedMemcpy((void *)(addr), &patched_addr, sizeof(uint32_t));
		}
		else if (*a == 0xFFFF0FA0) {
			l_debug("Patching %p -> __kuser_memory_barrier", (void *)a);
			patched_addr = 0x9A000FA0;
			kuKernelCpuUnrestrictedMemcpy((void *)(addr), &patched_addr, sizeof(uint32_t));
		}
	}
}

#define MOFLO_HTTP_STATUS_LIKE_CANCELLED 1

static int http_request_stub(void *url, int type, void *body,
                             void *out_response, int *out_response_code) {
    (void)url;
    (void)type;
    (void)body;
    (void)out_response;
    if (out_response_code) *out_response_code = 0;
    l_debug("[http-stub] HttpRequest -> failed (backend is dead)");
    return MOFLO_HTTP_STATUS_LIKE_CANCELLED;
}

static int http_request_headers_stub(void *url, int type, void *params,
                                     void *body, void *out_response,
                                     int *out_response_code, int flag) {
    (void)url;
    (void)type;
    (void)params;
    (void)body;
    (void)out_response;
    (void)flag;
    if (out_response_code) *out_response_code = 0;
    l_debug("[http-stub] HttpRequestWithHeaders -> failed (backend is dead)");
    return MOFLO_HTTP_STATUS_LIKE_CANCELLED;
}

static int http_is_connected_stub(void) {
    return 0;
}

static void hook_symbol(const char *symbol, uintptr_t dst) {
    uintptr_t addr = so_symbol(&so_mod, symbol);
    if (!addr) {
        l_warn("hook_symbol: \"%s\" not found, not hooked", symbol);
        return;
    }
    hook_addr(addr, dst);
}

static void http_patch(void) {
    hook_symbol("_ZN5moFlo15AndroidPlatform29SCHttpConnectionJavaInterface11IsConnectedEv",
                (uintptr_t)&http_is_connected_stub);
    hook_symbol("_ZN5moFlo15AndroidPlatform29SCHttpConnectionJavaInterface11HttpRequestESsNS0_17HTTP_REQUEST_TYPEESsRSsRi",
                (uintptr_t)&http_request_stub);
    hook_symbol("_ZN5moFlo15AndroidPlatform29SCHttpConnectionJavaInterface11HttpRequestESsNS0_17HTTP_REQUEST_TYPEENS_4Core15ParamDictionaryESsRSsRib",
                (uintptr_t)&http_request_headers_stub);
}

static void (*downloader_on_unzip_complete)(void) = NULL;

static void expansion_begin_download_stub(void) {
    l_debug("[expansion-stub] BeginDownloadingExpansions -> already unpacked");
    if (downloader_on_unzip_complete) {
        downloader_on_unzip_complete();
    }
}

static void expansion_patch(void) {
    downloader_on_unzip_complete = (void (*)(void))so_symbol(&so_mod,
            "_ZN26CGooglePlayDownloaderState15OnUnzipCompleteEb");
    if (!downloader_on_unzip_complete) {
        l_warn("expansion_patch: CGooglePlayDownloaderState::OnUnzipComplete "
               "not found, leaving the expansion downloader untouched");
        return;
    }
    hook_symbol("_ZN5MMCOH15AndroidPlatform30SCExpansionDownloaderInterface26BeginDownloadingExpansionsEv",
                (uintptr_t)&expansion_begin_download_stub);
}

void *(*game_cxa_current_exception_type)(void) = NULL;

void *(*game_cxa_get_globals)(void) = NULL;
const char *(*game_cegui_exception_what)(void *) = NULL;

static void cxx_exception_patch(void) {
	game_cxa_current_exception_type =
		(void *(*)(void)) so_symbol(&so_mod, "__cxa_current_exception_type");
	if (!game_cxa_current_exception_type) {
		l_warn("cxx_exception_patch: __cxa_current_exception_type not found, "
		       "uncaught C++ exceptions will log without a type name");
	}

	game_cxa_get_globals =
		(void *(*)(void)) so_symbol(&so_mod, "__cxa_get_globals");
	game_cegui_exception_what =
		(const char *(*)(void *)) so_symbol(&so_mod, "_ZNK5CEGUI9Exception4whatEv");
	if (!game_cxa_get_globals || !game_cegui_exception_what) {
		l_warn("cxx_exception_patch: exception-object/message symbols not fully "
		       "resolved, uncaught CEGUI exceptions will log without a message");
	}
}

static so_hook mapstate_set_input_mode_hook;

void *g_last_mapstate = NULL;

static void hook_MapState_setInputMode(void *self, int mode) {
	g_last_mapstate = self;
	SO_CONTINUE(int, mapstate_set_input_mode_hook, self, mode);
}

static void mapstate_tracking_patch(void) {
	uintptr_t set_input_mode_addr =
		so_symbol(&so_mod, "_ZN8MapState12setInputModeEi");
	if (set_input_mode_addr) {
		mapstate_set_input_mode_hook =
			hook_addr(set_input_mode_addr, (uintptr_t)&hook_MapState_setInputMode);
	} else {
		l_warn("mapstate_tracking_patch: MapState::setInputMode not found, "
			"map_dpad_is_active()/map_dpad_move() will not work");
	}
}

static void (*game_MapState_OnSwipeDirectionCallback)(void *, int) = NULL;

extern pthread_mutex_t g_engine_call_mutex;

static bool (*game_GameStateMachine_isStateTop)(void *) = NULL;

bool map_dpad_is_active(void) {
	return g_last_mapstate && game_GameStateMachine_isStateTop &&
	       game_GameStateMachine_isStateTop(g_last_mapstate);
}

int map_dpad_move(int direction) {
	if (!game_MapState_OnSwipeDirectionCallback || !g_last_mapstate) {
		return 0;
	}
	if (!map_dpad_is_active()) {
		return 0;
	}
	pthread_mutex_lock(&g_engine_call_mutex);
	game_MapState_OnSwipeDirectionCallback(g_last_mapstate, direction);
	pthread_mutex_unlock(&g_engine_call_mutex);
	return 1;
}

static void dpad_map_movement_patch(void) {
	game_MapState_OnSwipeDirectionCallback =
		(void (*)(void *, int)) so_symbol(&so_mod,
			"_ZN8MapState24OnSwipeDirectionCallbackE14SwipeDirection");
	if (!game_MapState_OnSwipeDirectionCallback) {
		l_warn("dpad_map_movement_patch: MapState::OnSwipeDirectionCallback not found");
	}
	game_GameStateMachine_isStateTop =
		(bool (*)(void *)) so_symbol(&so_mod, "_ZN16GameStateMachine10isStateTopEP8COHState");
	if (!game_GameStateMachine_isStateTop) {
		l_warn("dpad_map_movement_patch: GameStateMachine::isStateTop not found, "
			"map_dpad_is_active() will always report false");
	}
}

static void *g_last_battleintro = NULL;
static so_hook battleintro_init_control_info_hook;
static so_hook battleintro_destructor_hook;
static void (*game_MenuBattleIntroState_doSelectBattle)(void *) = NULL;
static void (*game_MenuBattleIntroState_doSelectFlee)(void *) = NULL;
static void (*game_MenuBattleIntroState_doSelectSetupUnits)(void *) = NULL;

static void hook_MenuBattleIntroState_initControlInfo(void *self) {
	g_last_battleintro = self;
	SO_CONTINUE(int, battleintro_init_control_info_hook, self);
}

static void hook_MenuBattleIntroState_Destructor(void *self) {
	if (g_last_battleintro == self) {
		g_last_battleintro = NULL;
	}
	SO_CONTINUE(int, battleintro_destructor_hook, self);
}

int battleintro_select(int which) {
	if (!g_last_battleintro) {
		return 0;
	}
	if (game_GameStateMachine_isStateTop &&
	    !game_GameStateMachine_isStateTop(g_last_battleintro)) {
		return 0;
	}
	void (*fn)(void *) = which == 0 ? game_MenuBattleIntroState_doSelectBattle :
	                      which == 1 ? game_MenuBattleIntroState_doSelectFlee :
	                                   game_MenuBattleIntroState_doSelectSetupUnits;
	if (!fn) {
		return 0;
	}
	pthread_mutex_lock(&g_engine_call_mutex);
	fn(g_last_battleintro);
	pthread_mutex_unlock(&g_engine_call_mutex);
	return 1;
}

static void battleintro_controls_patch(void) {
	uintptr_t init_control_info_addr =
		so_symbol(&so_mod, "_ZN20MenuBattleIntroState15initControlInfoEv");
	uintptr_t destructor_addr =
		so_symbol(&so_mod, "_ZN20MenuBattleIntroStateD1Ev");

	game_MenuBattleIntroState_doSelectBattle =
		(void (*)(void *)) so_symbol(&so_mod, "_ZN20MenuBattleIntroState14doSelectBattleEv");
	game_MenuBattleIntroState_doSelectFlee =
		(void (*)(void *)) so_symbol(&so_mod, "_ZN20MenuBattleIntroState12doSelectFleeEv");
	game_MenuBattleIntroState_doSelectSetupUnits =
		(void (*)(void *)) so_symbol(&so_mod, "_ZN20MenuBattleIntroState18doSelectSetupUnitsEv");
	game_GameStateMachine_isStateTop =
		(bool (*)(void *)) so_symbol(&so_mod, "_ZN16GameStateMachine10isStateTopEP8COHState");
	if (!game_GameStateMachine_isStateTop) {
		l_warn("battleintro_controls_patch: GameStateMachine::isStateTop not found, "
			"stale-press guard disabled");
	}

	if (init_control_info_addr) {
		battleintro_init_control_info_hook = hook_addr(init_control_info_addr,
			(uintptr_t)&hook_MenuBattleIntroState_initControlInfo);
	} else {
		l_warn("battleintro_controls_patch: MenuBattleIntroState::initControlInfo not found");
	}
	if (destructor_addr) {
		battleintro_destructor_hook = hook_addr(destructor_addr,
			(uintptr_t)&hook_MenuBattleIntroState_Destructor);
	} else {
		l_warn("battleintro_controls_patch: MenuBattleIntroState destructor not found");
	}
	if (!game_MenuBattleIntroState_doSelectBattle || !game_MenuBattleIntroState_doSelectFlee ||
	    !game_MenuBattleIntroState_doSelectSetupUnits) {
		l_warn("battleintro_controls_patch: one or more doSelect* symbols not found");
	}
}

static void *g_last_puzzlestate = NULL;
static so_hook puzzlestate_enter_hook;
static so_hook puzzlestate_destructor_hook;
static void *g_last_human = NULL;
static so_hook human_update_hook;
static so_hook human_destructor_hook;
static so_hook human_postaction_hook;

static void (*game_Human_postHumanAction)(void *, int, int, int) = NULL;
static int (*game_Player_canPullUnit)(void *, int) = NULL;
static int (*game_Player_canPushUnit)(void *, int) = NULL;
static void *(*game_GameBoard_getPullEntity)(void *, int, int) = NULL;
static int (*game_Entity_getRow)(void *) = NULL;
static int (*game_Player_hasControl)(void *) = NULL;
static int (*game_Player_canCastSpell)(void *) = NULL;
static void (*game_Human_RemoveSelectedSprite)(void *) = NULL;
static int (*game_Player_canFill)(void *) = NULL;
static int (*game_Player_canEndTurn)(void *) = NULL;

#define HUMAN_GAMEBOARD_OFFSET 0xde4
#define PLAYER_HELD_ENTITY_OFFSET 0xdf8
#define GAMEBOARD_NUMCOLS_OFFSET 0x4d4
#define HUMAN_SELECTED_COL_OFFSET 0xec4
#define HUMAN_SELECTED_ROW_OFFSET 0xec8
#define GAMEBOARD_NUMROWS_OFFSET 0x4d8

#define GAMEBOARD_CELL_STRIDE 0xc
#define GAMEBOARD_CELL_ARRAY_BASE 0x136

#define EXPECTED_SO_SIZE          7852600
#define EXPECTED_HUMAN_POSTACTION 0x004319ed
#define EXPECTED_PLAYER_CANPULL   0x004335bd

static bool g_struct_offsets_trusted = true;

static void battle_offsets_verify_binary(void) {
	SceIoStat st;
	SceOff size = 0;
	if (sceIoGetstat(SO_PATH, &st) >= 0)
		size = st.st_size;

	uintptr_t post = so_symbol(&so_mod, "_ZN5Human15postHumanActionEiii");
	uintptr_t pull = so_symbol(&so_mod, "_ZN6Player11canPullUnitEi");
	uint32_t post_val = post ? (uint32_t)(post - so_mod.load_addr) : 0;
	uint32_t pull_val = pull ? (uint32_t)(pull - so_mod.load_addr) : 0;

	if ((long long)size == EXPECTED_SO_SIZE &&
	    post_val == EXPECTED_HUMAN_POSTACTION &&
	    pull_val == EXPECTED_PLAYER_CANPULL) {
		return;
	}

	g_struct_offsets_trusted = false;
	l_error("battle offsets: libApplication.so is NOT the build these "
		"offsets came from (size %lld vs %d, Human::postHumanAction "
		"0x%08x vs 0x%08x, Player::canPullUnit 0x%08x vs 0x%08x). "
		"Battle D-pad/R1/Circle controls disabled rather than writing "
		"at seven wrong struct offsets.",
		(long long)size, EXPECTED_SO_SIZE,
		post_val, EXPECTED_HUMAN_POSTACTION,
		pull_val, EXPECTED_PLAYER_CANPULL);
}

static int g_battle_selected_col = 0;
static int g_battle_selected_row = 0;

static void *battle_entity_at_cell(void *board, int col, int row) {
	return *(void **)((uint8_t *)board +
		(col * GAMEBOARD_CELL_STRIDE + row + GAMEBOARD_CELL_ARRAY_BASE) * 4 + 4);
}

static void battle_sync_cursor_from_game(void *human, void *board) {
	int num_cols = *(int *)((uint8_t *)board + GAMEBOARD_NUMCOLS_OFFSET);
	int num_rows = *(int *)((uint8_t *)board + GAMEBOARD_NUMROWS_OFFSET);

	g_battle_selected_col = *(int *)((uint8_t *)human + HUMAN_SELECTED_COL_OFFSET);
	g_battle_selected_row = *(int *)((uint8_t *)human + HUMAN_SELECTED_ROW_OFFSET);

	if (g_battle_selected_col < 0) g_battle_selected_col = 0;
	if (num_cols > 0 && g_battle_selected_col >= num_cols) g_battle_selected_col = num_cols - 1;
	if (g_battle_selected_row < 0) g_battle_selected_row = 0;
	if (num_rows > 0 && g_battle_selected_row >= num_rows) g_battle_selected_row = num_rows - 1;
}

static void hook_PuzzleState_enter(void *self) {
	g_last_puzzlestate = self;
	g_battle_selected_col = 0;
	g_battle_selected_row = 0;
	SO_CONTINUE(int, puzzlestate_enter_hook, self);
}

static void hook_PuzzleState_Destructor(void *self) {
	if (g_last_puzzlestate == self) {
		g_last_puzzlestate = NULL;
	}
	SO_CONTINUE(int, puzzlestate_destructor_hook, self);
}

static void hook_Human_update(void *self, uint32_t dt_bits) {
	g_last_human = self;
	SO_CONTINUE(int, human_update_hook, self, dt_bits);
}

static void hook_Human_Destructor(void *self) {
	if (g_last_human == self) {
		g_last_human = NULL;
	}
	SO_CONTINUE(int, human_destructor_hook, self);
}

static bool prompt_popup_is_displayed(void);

bool battle_dpad_is_active(void) {
	return g_struct_offsets_trusted &&
	       g_last_puzzlestate && game_GameStateMachine_isStateTop &&
	       game_GameStateMachine_isStateTop(g_last_puzzlestate) &&
	       !prompt_popup_is_displayed();
}

static int g_battle_dragging = 0;
static int g_battle_origin_col = -1;

static void hook_Human_postHumanAction(void *self, int actionType, int cellIndex, int flags) {
	if (actionType == 0) {
		void *board = *(void **)((uint8_t *)self + HUMAN_GAMEBOARD_OFFSET);
		int num_cols = board ? *(int *)((uint8_t *)board + GAMEBOARD_NUMCOLS_OFFSET) : 0;
		if (num_cols > 0) {
			g_battle_selected_col = cellIndex / num_cols;
		}
		*(int *)((uint8_t *)self + HUMAN_SELECTED_COL_OFFSET) = g_battle_selected_col;
		g_battle_dragging = 1;
	} else if (actionType == 1) {
		g_battle_dragging = 0;
	}
	SO_CONTINUE(int, human_postaction_hook, self, actionType, cellIndex, flags);
}

int battle_column_move(int delta) {
	if (!battle_dpad_is_active() || !g_last_human || !game_Human_postHumanAction) {
		return 0;
	}
	pthread_mutex_lock(&g_engine_call_mutex);
	void *board = *(void **)((uint8_t *)g_last_human + HUMAN_GAMEBOARD_OFFSET);
	if (!board) {
		pthread_mutex_unlock(&g_engine_call_mutex);
		return 0;
	}
	int num_cols = *(int *)((uint8_t *)board + GAMEBOARD_NUMCOLS_OFFSET);
	if (num_cols <= 0) {
		pthread_mutex_unlock(&g_engine_call_mutex);
		return 0;
	}
	battle_sync_cursor_from_game(g_last_human, board);
	g_battle_selected_col += delta;
	if (g_battle_selected_col < 0) g_battle_selected_col = 0;
	if (g_battle_selected_col >= num_cols) g_battle_selected_col = num_cols - 1;

	*(int *)((uint8_t *)g_last_human + HUMAN_SELECTED_COL_OFFSET) = g_battle_selected_col;

	if (g_battle_dragging && game_Player_canPullUnit && game_GameBoard_getPullEntity &&
	    game_Entity_getRow) {
		int can_pull = game_Player_canPullUnit(g_last_human, g_battle_selected_col);
		if (can_pull) {
			void *entity = game_GameBoard_getPullEntity(board, g_battle_selected_col, -1);
			if (entity) {
				int row = game_Entity_getRow(entity);
				int cell_index = g_battle_selected_col * num_cols + row;
				game_Human_postHumanAction(g_last_human, 0, cell_index, 0);
			}
		}
	}
	pthread_mutex_unlock(&g_engine_call_mutex);
	return 1;
}

int battle_column_click(void) {
	if (!battle_dpad_is_active() || !g_last_human || !game_Human_postHumanAction) {
		return 0;
	}
	pthread_mutex_lock(&g_engine_call_mutex);
	void *board = *(void **)((uint8_t *)g_last_human + HUMAN_GAMEBOARD_OFFSET);
	if (!board) {
		pthread_mutex_unlock(&g_engine_call_mutex);
		return 0;
	}
	int num_cols = *(int *)((uint8_t *)board + GAMEBOARD_NUMCOLS_OFFSET);
	if (num_cols <= 0) {
		pthread_mutex_unlock(&g_engine_call_mutex);
		return 0;
	}

	battle_sync_cursor_from_game(g_last_human, board);

	if (!g_battle_dragging) {
		int can_pull = game_Player_canPullUnit ?
			game_Player_canPullUnit(g_last_human, g_battle_selected_col) : -1;
		if (can_pull > 0 && game_GameBoard_getPullEntity && game_Entity_getRow) {
			void *entity = game_GameBoard_getPullEntity(board, g_battle_selected_col, -1);
			if (entity) {
				int row = game_Entity_getRow(entity);
				int cell_index = g_battle_selected_col * num_cols + row;
				game_Human_postHumanAction(g_last_human, 0, cell_index, 0);
				g_battle_origin_col = g_battle_selected_col;
			}
		}
	} else {
		int can_push = game_Player_canPushUnit ?
			game_Player_canPushUnit(g_last_human, g_battle_selected_col) : -1;
		if (can_push > 0) {
			game_Human_postHumanAction(g_last_human, 1, g_battle_selected_col, 0);
		}
	}
	pthread_mutex_unlock(&g_engine_call_mutex);
	return 1;
}

int battle_column_cancel(void) {
	if (!battle_dpad_is_active()) {
		return 0;
	}
	if (!g_battle_dragging || g_battle_origin_col < 0 || !g_last_human ||
	    !game_Human_postHumanAction) {
		g_battle_dragging = 0;
		return 1;
	}
	pthread_mutex_lock(&g_engine_call_mutex);
	int can_push = game_Player_canPushUnit ?
		game_Player_canPushUnit(g_last_human, g_battle_origin_col) : -1;
	if (can_push > 0) {
		game_Human_postHumanAction(g_last_human, 1, g_battle_origin_col, 0);
	} else {
		l_warn("battle_column_cancel: origin column %d refused the return, "
			"dropping tracking flag anyway", g_battle_origin_col);
		g_battle_dragging = 0;
	}
	pthread_mutex_unlock(&g_engine_call_mutex);
	return 1;
}

static void (*game_PuzzleState_RequestToggleZoom)(void *) = NULL;

int battle_zoom_toggle(void) {
	if (!battle_dpad_is_active() || !game_PuzzleState_RequestToggleZoom) {
		return 0;
	}
	pthread_mutex_lock(&g_engine_call_mutex);
	game_PuzzleState_RequestToggleZoom(g_last_puzzlestate);
	pthread_mutex_unlock(&g_engine_call_mutex);
	return 1;
}

#define HUMAN_SPELL_LEVEL_OFFSET 0xe24
int battle_cast_spell(void) {
	if (!battle_dpad_is_active() || !g_last_human || !game_Human_postHumanAction ||
	    !game_Player_hasControl || !game_Player_canCastSpell) {
		return 0;
	}
	pthread_mutex_lock(&g_engine_call_mutex);
	int has_control = game_Player_hasControl(g_last_human);
	int can_cast = has_control ? game_Player_canCastSpell(g_last_human) : 0;
	void *board = *(void **)((uint8_t *)g_last_human + HUMAN_GAMEBOARD_OFFSET);
	const void *held = *(void **)((uint8_t *)g_last_human + PLAYER_HELD_ENTITY_OFFSET);
	if (has_control && !can_cast && held && !g_battle_dragging && board) {
		battle_sync_cursor_from_game(g_last_human, board);
		game_Human_postHumanAction(g_last_human, 1, g_battle_selected_col, 0);
	} else if (can_cast) {
		if (board) {
			battle_sync_cursor_from_game(g_last_human, board);
			game_Human_postHumanAction(g_last_human, 6, g_battle_selected_col, 0);
		}
		int level = *(int *)((uint8_t *)g_last_human + HUMAN_SPELL_LEVEL_OFFSET);
		game_Human_postHumanAction(g_last_human, 4, level, 0);
	}
	pthread_mutex_unlock(&g_engine_call_mutex);
	return 1;
}

int battle_row_move(int delta) {
	if (!battle_dpad_is_active() || !g_last_human) {
		return 0;
	}
	pthread_mutex_lock(&g_engine_call_mutex);
	void *board = *(void **)((uint8_t *)g_last_human + HUMAN_GAMEBOARD_OFFSET);
	if (!board) {
		pthread_mutex_unlock(&g_engine_call_mutex);
		return 0;
	}
	int num_rows = *(int *)((uint8_t *)board + GAMEBOARD_NUMROWS_OFFSET);
	if (num_rows <= 0) {
		pthread_mutex_unlock(&g_engine_call_mutex);
		return 0;
	}
	int num_cols = *(int *)((uint8_t *)board + GAMEBOARD_NUMCOLS_OFFSET);
	if (num_cols <= 0) {
		pthread_mutex_unlock(&g_engine_call_mutex);
		return 0;
	}
	battle_sync_cursor_from_game(g_last_human, board);
	void *from_entity = battle_entity_at_cell(board, g_battle_selected_col, g_battle_selected_row);
	int new_row = g_battle_selected_row + delta;
	while (from_entity && new_row >= 0 && new_row < num_rows &&
	       battle_entity_at_cell(board, g_battle_selected_col, new_row) == from_entity) {
		new_row += delta;
	}
	if (new_row < 0) new_row = 0;
	if (new_row >= num_rows) new_row = num_rows - 1;
	g_battle_selected_row = new_row;
	*(int *)((uint8_t *)g_last_human + HUMAN_SELECTED_ROW_OFFSET) = g_battle_selected_row;
	pthread_mutex_unlock(&g_engine_call_mutex);
	return 1;
}

int battle_kill(void) {
	if (!battle_dpad_is_active() || !g_last_human || !game_Human_RemoveSelectedSprite) {
		return 0;
	}
	pthread_mutex_lock(&g_engine_call_mutex);
	game_Human_RemoveSelectedSprite(g_last_human);
	pthread_mutex_unlock(&g_engine_call_mutex);
	return 1;
}

int battle_reinforcements(void) {
	if (!battle_dpad_is_active() || !g_last_human || !game_Human_postHumanAction ||
	    !game_Player_hasControl || !game_Player_canFill) {
		return 0;
	}
	pthread_mutex_lock(&g_engine_call_mutex);
	int has_control = game_Player_hasControl(g_last_human);
	int can_fill = has_control ? game_Player_canFill(g_last_human) : 0;
	if (can_fill) {
		game_Human_postHumanAction(g_last_human, 3, 0, 0);
	}
	pthread_mutex_unlock(&g_engine_call_mutex);
	return 1;
}

int battle_end_turn(void) {
	if (!battle_dpad_is_active() || !g_last_human || !game_Human_postHumanAction ||
	    !game_Player_hasControl || !game_Player_canEndTurn) {
		return 0;
	}
	pthread_mutex_lock(&g_engine_call_mutex);
	int has_control = game_Player_hasControl(g_last_human);
	int can_end_turn = has_control ? game_Player_canEndTurn(g_last_human) : 0;
	if (can_end_turn) {
		game_Human_postHumanAction(g_last_human, 17, 0, 0);
	}
	pthread_mutex_unlock(&g_engine_call_mutex);
	return 1;
}

#define IENGINE_DIALOGUESTATE_OFFSET   0xc4
#define IENGINE_MENUPROMPTSTATE_OFFSET 0x18c

static void *(*game_Engine_getInstancePtr)(void) = NULL;

static void *engine_state_if_top(int slot) {
	if (!g_struct_offsets_trusted || !game_Engine_getInstancePtr ||
	    !game_GameStateMachine_isStateTop) {
		return NULL;
	}
	void *engine = game_Engine_getInstancePtr();
	if (!engine) {
		return NULL;
	}
	void *state = *(void **)((uint8_t *)engine + slot);
	if (!state || !game_GameStateMachine_isStateTop(state)) {
		return NULL;
	}
	return state;
}

#define MAPSTATE_INPUT_MODE_OFFSET  0x15c
#define MAPSTATE_HERO_ACTOR_OFFSET  0x1a8
#define ACTOR_MOVE_STATE_OFFSET     0x3c
#define MAPNODE_INDEX_OFFSET        0x32

static void (*game_MapState_useNode)(void *, int, bool) = NULL;
static void *(*game_Actor_getCurrentNode)(void *) = NULL;

int map_interact(void) {
	if (!g_struct_offsets_trusted || !map_dpad_is_active() ||
	    !game_MapState_useNode || !game_Actor_getCurrentNode) {
		return 0;
	}
	int used = 0;
	pthread_mutex_lock(&g_engine_call_mutex);
	uint8_t *ms = (uint8_t *)g_last_mapstate;
	uint8_t *actor = *(uint8_t **)(ms + MAPSTATE_HERO_ACTOR_OFFSET);
	if (actor &&
	    *(int *)(ms + MAPSTATE_INPUT_MODE_OFFSET) != 0 &&
	    *(int *)(actor + ACTOR_MOVE_STATE_OFFSET) == 0) {
		uint8_t *node = game_Actor_getCurrentNode(actor);
		if (node) {
			int idx = *(uint16_t *)(node + MAPNODE_INDEX_OFFSET);
			l_debug("[map-interact] useNode(%d)", idx);
			game_MapState_useNode(ms, idx, false);
			used = 1;
		}
	}
	pthread_mutex_unlock(&g_engine_call_mutex);
	return used;
}

#define DIALOGUESTATE_ACTIVE_OFFSET      0x1c5
#define DIALOGUESTATE_CHOICE_OPEN_OFFSET 0x2c8
#define DIALOGUESTATE_CHOICE_MODE_OFFSET 0x2cc
#define DIALOGUESTATE_TEXT_OFFSET        0x308

static void (*game_DialogueState_doSelectAdvanceDialogue)(void *) = NULL;
static void (*game_DialogueState_choiceBox_select)(void *) = NULL;
static void (*game_DialogueState_choiceBox_cancel)(void *) = NULL;
static void (*game_DialogueState_choiceBoxVertical_select)(void *) = NULL;
static void (*game_DialogueState_choiceBoxVertical_cancel)(void *) = NULL;
static void (*game_DialogueState_choiceBoxNumber_select)(void *) = NULL;
static void (*game_DialogueState_choiceBoxNumber_cancel)(void *) = NULL;

static int dialogue_button_locked(bool confirm) {
	void *d = engine_state_if_top(IENGINE_DIALOGUESTATE_OFFSET);
	if (!d || !*((uint8_t *)d + DIALOGUESTATE_ACTIVE_OFFSET)) {
		return 0;
	}
	void (*fn)(void *) = NULL;
	if (*((uint8_t *)d + DIALOGUESTATE_CHOICE_OPEN_OFFSET)) {
		int mode = *(int *)((uint8_t *)d + DIALOGUESTATE_CHOICE_MODE_OFFSET);
		fn = mode == 0 ? (confirm ? game_DialogueState_choiceBox_select
		                          : game_DialogueState_choiceBox_cancel) :
		     mode == 1 ? (confirm ? game_DialogueState_choiceBoxVertical_select
		                          : game_DialogueState_choiceBoxVertical_cancel) :
		     mode == 2 ? (confirm ? game_DialogueState_choiceBoxNumber_select
		                          : game_DialogueState_choiceBoxNumber_cancel) : NULL;
	} else if (confirm) {
		if (!*(void **)((uint8_t *)d + DIALOGUESTATE_TEXT_OFFSET)) {
			l_debug("[dialogue] advance dropped: no BitmapText yet");
			return 1;
		}
		fn = game_DialogueState_doSelectAdvanceDialogue;
	}
	if (!fn) {
		return 0;
	}
	fn(d);
	return 1;
}

static int dialogue_button(bool confirm) {
	pthread_mutex_lock(&g_engine_call_mutex);
	int handled = dialogue_button_locked(confirm);
	pthread_mutex_unlock(&g_engine_call_mutex);
	return handled;
}

int dialogue_confirm(void) { return dialogue_button(true); }
int dialogue_cancel(void)  { return dialogue_button(false); }

#define MENUPROMPT_ANSWER_OFFSET 0x1f4
#define MENUPROMPT_OPEN_OFFSET   0x1f8
#define MENUPROMPT_YESNO_OFFSET  0x1f9
#define MENUPROMPT_VTABLE_HANDLEANSWER 0xac

static void (*game_MenuPromptState_doSelectOK)(void *) = NULL;

static uint8_t *prompt_state_if_displayed(void) {
	if (!g_struct_offsets_trusted || !game_Engine_getInstancePtr) {
		return NULL;
	}
	void *engine = game_Engine_getInstancePtr();
	if (!engine) {
		return NULL;
	}
	uint8_t *p = *(uint8_t **)((uint8_t *)engine + IENGINE_MENUPROMPTSTATE_OFFSET);
	if (!p || !p[MENUPROMPT_OPEN_OFFSET]) {
		return NULL;
	}
	return p;
}

static bool prompt_popup_is_displayed(void) {
	return prompt_state_if_displayed() != NULL;
}

static int prompt_answer_locked(int yes) {
	uint8_t *p = prompt_state_if_displayed();
	if (!p) {
		return 0;
	}
	if (*(int *)(p + MENUPROMPT_ANSWER_OFFSET) != 0) {
		return 1;
	}
	if (p[MENUPROMPT_YESNO_OFFSET]) {
		void (**vtable)(void *) = *(void (***)(void *))p;
		p[MENUPROMPT_OPEN_OFFSET] = 1;
		vtable[MENUPROMPT_VTABLE_HANDLEANSWER / sizeof(void *)](p);
		*(int *)(p + MENUPROMPT_ANSWER_OFFSET) = yes ? 1 : 2;
		l_debug("[prompt] answered %s", yes ? "yes" : "no");
	} else if (yes && game_MenuPromptState_doSelectOK) {
		game_MenuPromptState_doSelectOK(p);
		l_debug("[prompt] ok");
	}
	return 1;
}

int prompt_answer(int yes) {
	pthread_mutex_lock(&g_engine_call_mutex);
	int handled = prompt_answer_locked(yes);
	pthread_mutex_unlock(&g_engine_call_mutex);
	return handled;
}

#define DIALOGUESTATE_CHOICE_LIST_OFFSET 0x2e8
#define CHOICEBOX_INDEX_OFFSET           0x0
#define CHOICEBOX_COUNT_OFFSET           0x2
#define CHOICEBOX_DIRTY_OFFSET           0x4
#define CHOICEBOX_ARROW_UP_OFFSET        0x8
#define CHOICEBOX_ARROW_DOWN_OFFSET      0xc
#define CHOICEBOX_FIRST_VISIBLE_OFFSET   0x74
#define CHOICEBOX_VISIBLE_ROWS           3

static void (*game_DialogueState_choiceBox_scroll)(void *, bool) = NULL;
static int (*game_MultipleChoiceBox_moveIndex)(int, int, int, bool) = NULL;
static int (*game_MultipleChoiceBox_moveVisibleIndex)(int, int, int, int, int, bool) = NULL;
static void (*game_NewSprite_setVisible)(void *, bool) = NULL;

static uint8_t *dialogue_choice_box_if_mode(int mode) {
	uint8_t *d = engine_state_if_top(IENGINE_DIALOGUESTATE_OFFSET);
	if (!d || !d[DIALOGUESTATE_ACTIVE_OFFSET] || !d[DIALOGUESTATE_CHOICE_OPEN_OFFSET] ||
	    *(int *)(d + DIALOGUESTATE_CHOICE_MODE_OFFSET) != mode) {
		return NULL;
	}
	return d;
}

static int dialogue_choice_horizontal_locked(int to_yes) {
	uint8_t *d = dialogue_choice_box_if_mode(0);
	if (!d || !game_DialogueState_choiceBox_scroll) {
		return 0;
	}
	game_DialogueState_choiceBox_scroll(d, to_yes != 0);
	l_debug("[dialogue] highlight %s", to_yes ? "yes" : "no");
	return 1;
}

int dialogue_choice_horizontal(int to_yes) {
	pthread_mutex_lock(&g_engine_call_mutex);
	int handled = dialogue_choice_horizontal_locked(to_yes);
	pthread_mutex_unlock(&g_engine_call_mutex);
	return handled;
}

static int dialogue_choice_vertical_locked(int delta) {
	uint8_t *d = dialogue_choice_box_if_mode(1);
	if (!d || !game_MultipleChoiceBox_moveIndex || !game_MultipleChoiceBox_moveVisibleIndex ||
	    !game_NewSprite_setVisible) {
		return 0;
	}
	uint8_t *box = *(uint8_t **)(d + DIALOGUESTATE_CHOICE_LIST_OFFSET);
	int count = box ? *(int16_t *)(box + CHOICEBOX_COUNT_OFFSET) : 0;
	if (count >= 2) {
		int idx = game_MultipleChoiceBox_moveIndex(*(int16_t *)(box + CHOICEBOX_INDEX_OFFSET),
		                                           count, delta, false);
		*(int16_t *)(box + CHOICEBOX_INDEX_OFFSET) = (int16_t)idx;
		int first = game_MultipleChoiceBox_moveVisibleIndex(
			idx, *(int *)(box + CHOICEBOX_FIRST_VISIBLE_OFFSET), CHOICEBOX_VISIBLE_ROWS,
			count, delta, false);
		*(int *)(box + CHOICEBOX_FIRST_VISIBLE_OFFSET) = first;
		void *up = *(void **)(box + CHOICEBOX_ARROW_UP_OFFSET);
		void *down = *(void **)(box + CHOICEBOX_ARROW_DOWN_OFFSET);
		if (up) {
			game_NewSprite_setVisible(up, first > 0);
		}
		if (down) {
			game_NewSprite_setVisible(down, first + CHOICEBOX_VISIBLE_ROWS < count);
		}
		box[CHOICEBOX_DIRTY_OFFSET] = 1;
		l_debug("[dialogue] list row %d of %d", idx, count);
	}
	return 1;
}

int dialogue_choice_vertical(int delta) {
	pthread_mutex_lock(&g_engine_call_mutex);
	int handled = dialogue_choice_vertical_locked(delta);
	pthread_mutex_unlock(&g_engine_call_mutex);
	return handled;
}

#define IENGINE_COMPLETESTATE_OFFSET     0xcc
#define IENGINE_MENUUNITSELECT_OFFSET    0x11c
#define IENGINE_MENUDWELLINGSTATE_OFFSET 0x124
#define IENGINE_MENUQUESTSTATE_OFFSET    0x13c
#define IENGINE_MENUADVPAUSE_OFFSET      0x16c
#define IENGINE_MENUARTIFACTSTATE_OFFSET 0x144
#define IENGINE_MENUSUMMARYSTATE_OFFSET  0x14c
#define IENGINE_MENUHEROSTATE_OFFSET     0x154
#define COMPLETESTATE_TYPE_OFFSET        0x1f0
#define COMPLETESTATE_TYPE_NO_CLOSE      4

#define COMPLETESTATE_MSG_NAV_OFFSET     0x1f4
#define COMPLETESTATE_YESNO_NAV_OFFSET   0x1f8
#define COMPLETESTATE_TYPE_EQUIP_UNIT     0
#define COMPLETESTATE_TYPE_EQUIP_ARTIFACT 1

static void (*game_CompleteState_doSelectOK_equipUnit)(void *) = NULL;
static void (*game_CompleteState_doSelectCancel_equipUnit)(void *) = NULL;
static void (*game_CompleteState_doSelectOK_equipUnitMessage)(void *) = NULL;
static void (*game_CompleteState_doSelectOK_equipArtifact)(void *) = NULL;
static void (*game_CompleteState_doSelectCancel_equipArtifact)(void *) = NULL;
static void (*game_CompleteState_doSelectOK_equipArtifactMessage)(void *) = NULL;
static int  (*game_Behaviour_hasFocus)(void *, int) = NULL;
static int  (*game_Player_getMuliplayerBattlePlayerID)(int) = NULL;

static bool completestate_yesno_has_focus(uint8_t *p) {
	void *nav = *(void **)(p + COMPLETESTATE_YESNO_NAV_OFFSET);
	if (!nav || !game_Behaviour_hasFocus) {
		return false;
	}
	int player = game_Player_getMuliplayerBattlePlayerID ?
		game_Player_getMuliplayerBattlePlayerID(0) : 0;
	return game_Behaviour_hasFocus(nav, player) != 0;
}

static struct screen_close {
	int slot;
	const char *name;
	const char *symbol;
	const char *accept_symbol;
	void (*fn)(void *);
	void (*accept_fn)(void *);
} screen_closes[] = {
	{ IENGINE_COMPLETESTATE_OFFSET,     "CompleteState",     "_ZN13CompleteState13doSelectCloseEv",   NULL, NULL, NULL },
	{ IENGINE_MENUSUMMARYSTATE_OFFSET,  "MenuSummaryState",  "_ZN16MenuSummaryState12doSelectNextEv", NULL, NULL, NULL },
	{ IENGINE_MENUHEROSTATE_OFFSET,     "MenuHeroState",     "_ZN13MenuHeroState8OnGoBackEv",         NULL, NULL, NULL },
	{ IENGINE_MENUQUESTSTATE_OFFSET,    "MenuQuestState",    "_ZN14MenuQuestState13doSelectCloseEv",  NULL, NULL, NULL },
	{ IENGINE_MENUARTIFACTSTATE_OFFSET, "MenuArtifactState", "_ZN17MenuArtifactState8OnGoBackEv",     NULL, NULL, NULL },
	{ IENGINE_MENUUNITSELECT_OFFSET,    "MenuUnitSelect",    "_ZN14MenuUnitSelect8OnGoBackEv",        NULL, NULL, NULL },
	{ IENGINE_MENUDWELLINGSTATE_OFFSET, "MenuDwellingState", "_ZN17MenuDwellingState8OnGoBackEv",
	  "_ZN17MenuDwellingState16doRequestBuyUnitEv", NULL, NULL },
	{ IENGINE_MENUADVPAUSE_OFFSET,      "MenuAdvPause",      "_ZN12MenuAdvPause8OnGoBackEv",     NULL, NULL, NULL },
};
#define NUM_SCREEN_CLOSES (sizeof(screen_closes) / sizeof(screen_closes[0]))

static void (*game_CompleteState_doSelectOK)(void *) = NULL;

static int screen_close_locked(int accept) {
	if (prompt_popup_is_displayed()) {
		return 0;
	}
	for (size_t i = 0; i < NUM_SCREEN_CLOSES; i++) {
		struct screen_close *e = &screen_closes[i];
		uint8_t *p = engine_state_if_top(e->slot);
		if (!p) {
			continue;
		}
		void (*fn)(void *) = accept && e->accept_fn ? e->accept_fn : e->fn;
		if (e->slot == IENGINE_COMPLETESTATE_OFFSET) {
			int type = *(int *)(p + COMPLETESTATE_TYPE_OFFSET);
			int equip_unit = type == COMPLETESTATE_TYPE_EQUIP_UNIT;
			if (type <= COMPLETESTATE_TYPE_EQUIP_ARTIFACT) {
				if (completestate_yesno_has_focus(p)) {
					fn = accept ?
						(equip_unit ? game_CompleteState_doSelectOK_equipUnit
						            : game_CompleteState_doSelectOK_equipArtifact) :
						(equip_unit ? game_CompleteState_doSelectCancel_equipUnit
						            : game_CompleteState_doSelectCancel_equipArtifact);
				} else if (accept) {
					fn = equip_unit ?
						game_CompleteState_doSelectOK_equipUnitMessage :
						game_CompleteState_doSelectOK_equipArtifactMessage;
				}
				if (!fn) {
					fn = e->fn;
				}
			} else if (accept && game_CompleteState_doSelectOK) {
				fn = game_CompleteState_doSelectOK;
			} else if (!accept && type == COMPLETESTATE_TYPE_NO_CLOSE) {
				fn = NULL;
			}
		}
		if (fn) {
			fn(p);
			l_debug("[screen] %s %s", e->name, accept ? "accept" : "close");
		}
		return 1;
	}
	return 0;
}

int screen_close(int accept) {
	pthread_mutex_lock(&g_engine_call_mutex);
	int handled = screen_close_locked(accept);
	pthread_mutex_unlock(&g_engine_call_mutex);
	return handled;
}

#define IENGINE_BOOKENDSTATE_OFFSET 0xdc
#define BOOKENDSTATE_ADVANCE_OFFSET 0x180

int bookend_next(void) {
	pthread_mutex_lock(&g_engine_call_mutex);
	uint8_t *b = engine_state_if_top(IENGINE_BOOKENDSTATE_OFFSET);
	if (b) {
		b[BOOKENDSTATE_ADVANCE_OFFSET] = 1;
		l_debug("[screen] BookendState next");
	}
	pthread_mutex_unlock(&g_engine_call_mutex);
	return b != NULL;
}

#define IENGINE_MENUADVDEATH_OFFSET    0x174
#define MENUADVDEATH_QUIT_ASKED_OFFSET 0x18c
#define MENUADVDEATH_QUIT_DONE_OFFSET  0x18d

static void (*game_MenuAdvDeath_doSelectResumeGame)(void *) = NULL;
static void (*game_MenuAdvDeath_doSelectQuitGame)(void *) = NULL;

int advdeath_select(int resume) {
	void (*fn)(void *) = resume ? game_MenuAdvDeath_doSelectResumeGame
	                            : game_MenuAdvDeath_doSelectQuitGame;
	if (!fn) {
		return 0;
	}
	pthread_mutex_lock(&g_engine_call_mutex);
	uint8_t *d = prompt_popup_is_displayed() ?
		NULL : engine_state_if_top(IENGINE_MENUADVDEATH_OFFSET);
	if (d && (d[MENUADVDEATH_QUIT_ASKED_OFFSET] || d[MENUADVDEATH_QUIT_DONE_OFFSET])) {
		d = NULL;
	}
	if (d) {
		fn(d);
		l_debug("[screen] MenuAdvDeath %s", resume ? "continue" : "quit");
	}
	pthread_mutex_unlock(&g_engine_call_mutex);
	return d != NULL;
}

static void (*game_MenuDwellingState_doRequestChangeBuyAmount)(void *, int) = NULL;

int dwelling_change_amount(int delta) {
	if (!game_MenuDwellingState_doRequestChangeBuyAmount) {
		return 0;
	}
	pthread_mutex_lock(&g_engine_call_mutex);
	uint8_t *d = prompt_popup_is_displayed() ?
		NULL : engine_state_if_top(IENGINE_MENUDWELLINGSTATE_OFFSET);
	if (d) {
		game_MenuDwellingState_doRequestChangeBuyAmount(d, delta);
		l_debug("[screen] MenuDwellingState buy amount %+d", delta);
	}
	pthread_mutex_unlock(&g_engine_call_mutex);
	return d != NULL;
}

#define MAPSTATE_PAUSE_SPRITE_OFFSET 0x230
#define MAPSTATE_QUEST_SPRITE_OFFSET 0x234
#define MAPSTATE_HUD_BUSY_OFFSET     0x1f0
#define MAPSTATE_TAP_MOVED_OFFSET    0x19e
#define SHARED_PTR_COUNT_OFFSET      4

static void (*game_IEngine_pushState)(void *, void *) = NULL;
static void *(*game_shared_count_copy)(void *, const void *) = NULL;
static void (*game_shared_count_dtor)(void *) = NULL;
static int (*game_Profile_getAdventureProfileStat)(int) = NULL;
static void (*game_MenuAdvPause_setPlayerID)(void *, int) = NULL;
static void (*game_MenuAdvPause_setShowStatsOnly)(void *, bool) = NULL;

static int engine_push_pooled_state(void *engine, int slot) {
	if (!game_IEngine_pushState || !game_shared_count_copy || !game_shared_count_dtor) {
		return 0;
	}
	struct { void *px; void *pn; } sp;
	uint8_t *e = (uint8_t *)engine;
	sp.px = *(void **)(e + slot);
	if (!sp.px) {
		return 0;
	}
	game_shared_count_copy(&sp.pn, e + slot + SHARED_PTR_COUNT_OFFSET);
	game_IEngine_pushState(engine, &sp);
	game_shared_count_dtor(&sp.pn);
	return 1;
}

static uint8_t *map_hud_button_target(void) {
	if (!g_struct_offsets_trusted || !map_dpad_is_active()) {
		return NULL;
	}
	uint8_t *ms = (uint8_t *)g_last_mapstate;
	uint8_t *actor = *(uint8_t **)(ms + MAPSTATE_HERO_ACTOR_OFFSET);
	if (!actor || *(int *)(actor + ACTOR_MOVE_STATE_OFFSET) != 0 ||
	    *(int *)(ms + MAPSTATE_HUD_BUSY_OFFSET) != 0) {
		return NULL;
	}
	return ms;
}

int map_quest_menu(void) {
	if (!game_Engine_getInstancePtr || !game_Profile_getAdventureProfileStat) {
		return 0;
	}
	int pushed = 0;
	pthread_mutex_lock(&g_engine_call_mutex);
	uint8_t *ms = map_hud_button_target();
	if (ms && *(void **)(ms + MAPSTATE_QUEST_SPRITE_OFFSET) &&
	    *(int *)(ms + MAPSTATE_INPUT_MODE_OFFSET) == 1 &&
	    game_Profile_getAdventureProfileStat(0) != 0) {
		void *engine = game_Engine_getInstancePtr();
		if (engine) {
			pushed = engine_push_pooled_state(engine, IENGINE_MENUQUESTSTATE_OFFSET);
		}
		if (pushed) {
			l_debug("[map-hud] quest log");
		}
	}
	pthread_mutex_unlock(&g_engine_call_mutex);
	return pushed;
}

int map_pause_menu(void) {
	if (!game_Engine_getInstancePtr || !game_MenuAdvPause_setPlayerID ||
	    !game_MenuAdvPause_setShowStatsOnly) {
		return 0;
	}
	int pushed = 0;
	pthread_mutex_lock(&g_engine_call_mutex);
	uint8_t *ms = map_hud_button_target();
	if (ms && *(void **)(ms + MAPSTATE_PAUSE_SPRITE_OFFSET) &&
	    *(int *)(ms + MAPSTATE_INPUT_MODE_OFFSET) != 0) {
		void *engine = game_Engine_getInstancePtr();
		void *pause = engine ?
			*(void **)((uint8_t *)engine + IENGINE_MENUADVPAUSE_OFFSET) : NULL;
		if (pause) {
			game_MenuAdvPause_setPlayerID(pause, 0);
			game_MenuAdvPause_setShowStatsOnly(pause, false);
			pushed = engine_push_pooled_state(engine, IENGINE_MENUADVPAUSE_OFFSET);
		}
		if (pushed) {
			ms[MAPSTATE_TAP_MOVED_OFFSET] = 0;
			l_debug("[map-hud] pause menu");
		}
	}
	pthread_mutex_unlock(&g_engine_call_mutex);
	return pushed;
}

static void gameplay_buttons_patch(void) {
	game_Engine_getInstancePtr =
		(void *(*)(void)) so_symbol(&so_mod, "_ZN6Engine14getInstancePtrEv");
	game_MapState_useNode =
		(void (*)(void *, int, bool)) so_symbol(&so_mod, "_ZN8MapState7useNodeEib");
	game_Actor_getCurrentNode =
		(void *(*)(void *)) so_symbol(&so_mod, "_ZN5Actor14getCurrentNodeEv");
	game_DialogueState_doSelectAdvanceDialogue =
		(void (*)(void *)) so_symbol(&so_mod, "_ZN13DialogueState23doSelectAdvanceDialogueEv");
	game_DialogueState_choiceBox_select =
		(void (*)(void *)) so_symbol(&so_mod, "_ZN13DialogueState16choiceBox_selectEv");
	game_DialogueState_choiceBox_cancel =
		(void (*)(void *)) so_symbol(&so_mod, "_ZN13DialogueState16choiceBox_cancelEv");
	game_DialogueState_choiceBoxVertical_select =
		(void (*)(void *)) so_symbol(&so_mod, "_ZN13DialogueState24choiceBoxVertical_selectEv");
	game_DialogueState_choiceBoxVertical_cancel =
		(void (*)(void *)) so_symbol(&so_mod, "_ZN13DialogueState24choiceBoxVertical_cancelEv");
	game_DialogueState_choiceBoxNumber_select =
		(void (*)(void *)) so_symbol(&so_mod, "_ZN13DialogueState22choiceBoxNumber_selectEv");
	game_DialogueState_choiceBoxNumber_cancel =
		(void (*)(void *)) so_symbol(&so_mod, "_ZN13DialogueState22choiceBoxNumber_cancelEv");
	game_MenuPromptState_doSelectOK =
		(void (*)(void *)) so_symbol(&so_mod, "_ZN15MenuPromptState10doSelectOKEv");
	game_DialogueState_choiceBox_scroll =
		(void (*)(void *, bool)) so_symbol(&so_mod, "_ZN13DialogueState16choiceBox_scrollEb");
	game_MultipleChoiceBox_moveIndex =
		(int (*)(int, int, int, bool)) so_symbol(&so_mod, "_ZN17MultipleChoiceBox9moveIndexEiiib");
	game_MultipleChoiceBox_moveVisibleIndex =
		(int (*)(int, int, int, int, int, bool)) so_symbol(&so_mod, "_ZN17MultipleChoiceBox16moveVisibleIndexEiiiiib");
	game_NewSprite_setVisible =
		(void (*)(void *, bool)) so_symbol(&so_mod, "_ZN9NewSprite10setVisibleEb");
	game_CompleteState_doSelectOK =
		(void (*)(void *)) so_symbol(&so_mod, "_ZN13CompleteState10doSelectOKEv");
	game_CompleteState_doSelectOK_equipUnit =
		(void (*)(void *)) so_symbol(&so_mod, "_ZN13CompleteState20doSelectOK_equipUnitEv");
	game_CompleteState_doSelectCancel_equipUnit =
		(void (*)(void *)) so_symbol(&so_mod, "_ZN13CompleteState24doSelectCancel_equipUnitEv");
	game_CompleteState_doSelectOK_equipUnitMessage =
		(void (*)(void *)) so_symbol(&so_mod, "_ZN13CompleteState27doSelectOK_equipUnitMessageEv");
	game_CompleteState_doSelectOK_equipArtifact =
		(void (*)(void *)) so_symbol(&so_mod, "_ZN13CompleteState24doSelectOK_equipArtifactEv");
	game_CompleteState_doSelectCancel_equipArtifact =
		(void (*)(void *)) so_symbol(&so_mod, "_ZN13CompleteState28doSelectCancel_equipArtifactEv");
	game_CompleteState_doSelectOK_equipArtifactMessage =
		(void (*)(void *)) so_symbol(&so_mod,
			"_ZN13CompleteState31doSelectOK_equipArtifactMessageEv");
	game_Behaviour_hasFocus =
		(int (*)(void *, int)) so_symbol(&so_mod, "_ZN9Behaviour8hasFocusEi");
	game_Player_getMuliplayerBattlePlayerID =
		(int (*)(int)) so_symbol(&so_mod, "_ZN6Player27getMuliplayerBattlePlayerIDEi");
	if (!game_CompleteState_doSelectOK_equipUnit || !game_Behaviour_hasFocus) {
		l_warn("gameplay_buttons_patch: CompleteState's equip dialog is not "
			"fully resolved, X there will only close the screen");
	}
	game_MenuDwellingState_doRequestChangeBuyAmount =
		(void (*)(void *, int)) so_symbol(&so_mod,
			"_ZN17MenuDwellingState24doRequestChangeBuyAmountEi");
	if (!game_MenuDwellingState_doRequestChangeBuyAmount) {
		l_warn("gameplay_buttons_patch: MenuDwellingState::doRequestChangeBuyAmount "
			"not found, the dwelling +/- buttons will no-op");
	}
	game_IEngine_pushState =
		(void (*)(void *, void *)) so_symbol(&so_mod,
			"_ZN7IEngine9pushStateERKN5boost10shared_ptrI8COHStateEE");
	game_shared_count_copy =
		(void *(*)(void *, const void *)) so_symbol(&so_mod,
			"_ZN5boost6detail12shared_countC1ERKS1_");
	game_shared_count_dtor =
		(void (*)(void *)) so_symbol(&so_mod, "_ZN5boost6detail12shared_countD1Ev");
	game_Profile_getAdventureProfileStat =
		(int (*)(int)) so_symbol(&so_mod,
			"_ZN7Profile23getAdventureProfileStatE19t_adventureStatType");
	game_MenuAdvPause_setPlayerID =
		(void (*)(void *, int)) so_symbol(&so_mod, "_ZN12MenuAdvPause11setPlayerIDEi");
	game_MenuAdvPause_setShowStatsOnly =
		(void (*)(void *, bool)) so_symbol(&so_mod, "_ZN12MenuAdvPause16setShowStatsOnlyEb");
	if (!game_IEngine_pushState || !game_shared_count_copy || !game_shared_count_dtor) {
		l_warn("gameplay_buttons_patch: IEngine::pushState/boost::shared_count not "
			"found, the map's L and R buttons will no-op");
	}
	if (!game_Profile_getAdventureProfileStat) {
		l_warn("gameplay_buttons_patch: Profile::getAdventureProfileStat not found, "
			"the map's quest-log button will no-op");
	}
	if (!game_MenuAdvPause_setPlayerID || !game_MenuAdvPause_setShowStatsOnly) {
		l_warn("gameplay_buttons_patch: MenuAdvPause::setPlayerID/setShowStatsOnly not "
			"found, the map's pause button will no-op");
	}
	for (size_t i = 0; i < NUM_SCREEN_CLOSES; i++) {
		screen_closes[i].fn = (void (*)(void *)) so_symbol(&so_mod, screen_closes[i].symbol);
		if (!screen_closes[i].fn) {
			l_warn("gameplay_buttons_patch: %s not found, %s close button will no-op",
				screen_closes[i].symbol, screen_closes[i].name);
		}
		if (!screen_closes[i].accept_symbol) {
			continue;
		}
		screen_closes[i].accept_fn =
			(void (*)(void *)) so_symbol(&so_mod, screen_closes[i].accept_symbol);
		if (!screen_closes[i].accept_fn) {
			l_warn("gameplay_buttons_patch: %s not found, %s Cross will fall back "
				"to its close button", screen_closes[i].accept_symbol,
				screen_closes[i].name);
		}
	}
	if (!game_Engine_getInstancePtr) {
		l_warn("gameplay_buttons_patch: Engine::getInstancePtr not found, "
			"dialogue/prompt buttons will no-op");
	}
	if (!game_MapState_useNode || !game_Actor_getCurrentNode) {
		l_warn("gameplay_buttons_patch: MapState::useNode/Actor::getCurrentNode "
			"not found, map interact button will no-op");
	}
	if (!game_DialogueState_doSelectAdvanceDialogue || !game_DialogueState_choiceBox_select ||
	    !game_DialogueState_choiceBox_cancel || !game_DialogueState_choiceBoxVertical_select ||
	    !game_DialogueState_choiceBoxVertical_cancel ||
	    !game_DialogueState_choiceBoxNumber_select ||
	    !game_DialogueState_choiceBoxNumber_cancel) {
		l_warn("gameplay_buttons_patch: one or more DialogueState symbols not found");
	}
	if (!game_MenuPromptState_doSelectOK) {
		l_warn("gameplay_buttons_patch: MenuPromptState::doSelectOK not found");
	}
	if (!game_DialogueState_choiceBox_scroll || !game_MultipleChoiceBox_moveIndex ||
	    !game_MultipleChoiceBox_moveVisibleIndex || !game_NewSprite_setVisible) {
		l_warn("gameplay_buttons_patch: choice-box navigation symbols not found, "
			"D-pad on dialogue choices will no-op");
	}
	if (!game_CompleteState_doSelectOK) {
		l_warn("gameplay_buttons_patch: CompleteState::doSelectOK not found");
	}
	game_MenuAdvDeath_doSelectResumeGame =
		(void (*)(void *)) so_symbol(&so_mod, "_ZN12MenuAdvDeath18doSelectResumeGameEv");
	game_MenuAdvDeath_doSelectQuitGame =
		(void (*)(void *)) so_symbol(&so_mod, "_ZN12MenuAdvDeath16doSelectQuitGameEv");
	if (!game_MenuAdvDeath_doSelectResumeGame || !game_MenuAdvDeath_doSelectQuitGame) {
		l_warn("gameplay_buttons_patch: MenuAdvDeath::doSelectResumeGame/doSelectQuitGame "
			"not found, the defeat screen's buttons will no-op");
	}
}

static void battle_column_shift_patch(void) {
	battle_offsets_verify_binary();

	uintptr_t puzzlestate_enter_addr = so_symbol(&so_mod, "_ZN11PuzzleState5enterEv");
	uintptr_t puzzlestate_destructor_addr = so_symbol(&so_mod, "_ZN11PuzzleStateD1Ev");
	uintptr_t human_update_addr = so_symbol(&so_mod, "_ZN5Human6updateEf");
	uintptr_t human_destructor_addr = so_symbol(&so_mod, "_ZN5HumanD1Ev");

	game_Human_postHumanAction =
		(void (*)(void *, int, int, int)) so_symbol(&so_mod, "_ZN5Human15postHumanActionEiii");
	game_Player_canPullUnit =
		(int (*)(void *, int)) so_symbol(&so_mod, "_ZN6Player11canPullUnitEi");
	game_Player_canPushUnit =
		(int (*)(void *, int)) so_symbol(&so_mod, "_ZN6Player11canPushUnitEi");
	game_GameBoard_getPullEntity =
		(void *(*)(void *, int, int)) so_symbol(&so_mod, "_ZN9GameBoard13getPullEntityEi20t_playerIndexOnBoard");
	game_Entity_getRow =
		(int (*)(void *)) so_symbol(&so_mod, "_ZN6Entity6getRowEv");
	game_GameStateMachine_isStateTop =
		(bool (*)(void *)) so_symbol(&so_mod, "_ZN16GameStateMachine10isStateTopEP8COHState");
	game_PuzzleState_RequestToggleZoom =
		(void (*)(void *)) so_symbol(&so_mod, "_ZN11PuzzleState17RequestToggleZoomEv");
	if (!game_PuzzleState_RequestToggleZoom) {
		l_warn("battle_column_shift_patch: PuzzleState::RequestToggleZoom not found");
	}
	game_Player_hasControl =
		(int (*)(void *)) so_symbol(&so_mod, "_ZN6Player10hasControlEv");
	game_Player_canCastSpell =
		(int (*)(void *)) so_symbol(&so_mod, "_ZN6Player12canCastSpellEv");
	if (!game_Player_hasControl || !game_Player_canCastSpell) {
		l_warn("battle_column_shift_patch: hasControl/canCastSpell not found, "
			"hero special ability button will no-op");
	}
	game_Human_RemoveSelectedSprite =
		(void (*)(void *)) so_symbol(&so_mod, "_ZN5Human20RemoveSelectedSpriteEv");
	if (!game_Human_RemoveSelectedSprite) {
		l_warn("battle_column_shift_patch: RemoveSelectedSprite not found, "
			"kill/delete button will no-op");
	}
	game_Player_canFill =
		(int (*)(void *)) so_symbol(&so_mod, "_ZN6Player7canFillEv");
	game_Player_canEndTurn =
		(int (*)(void *)) so_symbol(&so_mod, "_ZN6Player10canEndTurnEv");
	if (!game_Player_canFill || !game_Player_canEndTurn) {
		l_warn("battle_column_shift_patch: canFill/canEndTurn not found, "
			"reinforcements/end-turn buttons will no-op");
	}

	if (puzzlestate_enter_addr) {
		puzzlestate_enter_hook = hook_addr(puzzlestate_enter_addr,
			(uintptr_t)&hook_PuzzleState_enter);
	} else {
		l_warn("battle_column_shift_patch: PuzzleState::enter not found");
	}
	if (puzzlestate_destructor_addr) {
		puzzlestate_destructor_hook = hook_addr(puzzlestate_destructor_addr,
			(uintptr_t)&hook_PuzzleState_Destructor);
	} else {
		l_warn("battle_column_shift_patch: PuzzleState destructor not found");
	}
	if (human_update_addr) {
		human_update_hook = hook_addr(human_update_addr, (uintptr_t)&hook_Human_update);
	} else {
		l_warn("battle_column_shift_patch: Human::update not found");
	}
	if (human_destructor_addr) {
		human_destructor_hook = hook_addr(human_destructor_addr, (uintptr_t)&hook_Human_Destructor);
	} else {
		l_warn("battle_column_shift_patch: Human destructor not found");
	}
	if (game_Human_postHumanAction && g_struct_offsets_trusted) {
		human_postaction_hook = hook_addr((uintptr_t)game_Human_postHumanAction,
			(uintptr_t)&hook_Human_postHumanAction);
	} else if (!game_Human_postHumanAction) {
		l_warn("battle_column_shift_patch: Human::postHumanAction not found, "
			"can't hook it");
	}
	if (!game_Human_postHumanAction || !game_Player_canPullUnit || !game_Player_canPushUnit ||
	    !game_GameBoard_getPullEntity || !game_Entity_getRow) {
		l_warn("battle_column_shift_patch: one or more direct-call symbols not found");
	}
}

static void hook_MenuTitleState_doSelectMultiplayerEntryAction(void *self) {
	l_info("[battle-mode] Battle Mode tapped - no-op (online multiplayer "
		"not implemented, avoiding the BattleLobby imageset crash)");
}

static void battle_mode_noop_patch(void) {
	uintptr_t addr = so_symbol(&so_mod,
		"_ZN14MenuTitleState30doSelectMultiplayerEntryActionEv");
	if (addr) {
		hook_addr(addr, (uintptr_t)&hook_MenuTitleState_doSelectMultiplayerEntryAction);
	} else {
		l_warn("battle_mode_noop_patch: MenuTitleState::doSelectMultiplayerEntryAction "
			"not found, Battle Mode button will still crash");
	}
}

static so_hook fmod_errorcheck_hook;
static unsigned fmod_notready_swallowed;

static int hook_CFMODSystem_ErrorCheck(void *self, int result) {
	if (result == 33)
		return SO_CONTINUE(int, fmod_errorcheck_hook, self, 0);
	if (result == 54) {
		if (++fmod_notready_swallowed <= 8)
			l_warn("[music] ErrorCheck: swallowed FMOD 54 (not ready), #%u",
			       fmod_notready_swallowed);
		return SO_CONTINUE(int, fmod_errorcheck_hook, self, 0);
	}
	return SO_CONTINUE(int, fmod_errorcheck_hook, self, result);
}

static void fmod_errorcheck_patch(void) {
	uintptr_t errcheck_addr = so_symbol(&so_mod, "_ZN5moFlo15AndroidPlatform11CFMODSystem10ErrorCheckE11FMOD_RESULT");
	if (errcheck_addr)
		fmod_errorcheck_hook = hook_addr(errcheck_addr, (uintptr_t)&hook_CFMODSystem_ErrorCheck);
	else
		l_error("fmod_errorcheck_patch: CFMODSystem::ErrorCheck not found");
}

static so_hook fmod_init_hook;
static so_hook fmod_close_hook;
typedef int (*fmod_init_t)(void *self, int maxchannels, unsigned int flags, void *extradriverdata);

static int hook_fmod_init(void *self, int maxchannels, unsigned int flags, void *extradriverdata) {
	int ret = SO_CONTINUE(int, fmod_init_hook, self, maxchannels, flags, extradriverdata);
	if (ret == 0) {
		audio_pump_start(self);
	}
	return ret;
}

static int hook_fmod_close(void *self) {
	audio_pump_stop();
	return SO_CONTINUE(int, fmod_close_hook, self);
}

static void fmod_audio_pump_patch(void) {
	uintptr_t init_addr = so_symbol(&so_mod_fmodex, "_ZN4FMOD6System4initEijPv");
	uintptr_t close_addr = so_symbol(&so_mod_fmodex, "_ZN4FMOD6System5closeEv");
	if (init_addr)
		fmod_init_hook = hook_addr(init_addr, (uintptr_t)&hook_fmod_init);
	else
		l_error("fmod_audio_pump_patch: FMOD::System::init symbol not found - audio will be silent");
	if (close_addr)
		fmod_close_hook = hook_addr(close_addr, (uintptr_t)&hook_fmod_close);
	else
		l_error("fmod_audio_pump_patch: FMOD::System::close symbol not found");
}

static void texture_budget_patch(void) {
	if (TEXTURE_BUDGET_MB == 0)
		return;
	if (TEXTURE_BUDGET_MB > 255) {
		l_error("[texbudget] %d MB does not fit `movs #imm8` - not patched",
		        TEXTURE_BUDGET_MB);
		return;
	}
	uintptr_t fn = so_symbol(&so_mod,
		"_ZN17TextureManager_v26createEPKcjbN5moFlo4Core6CImage6FormatEb");
	if (!fn) {
		l_error("[texbudget] TextureManager_v2::create not found - not patched");
		return;
	}
	uintptr_t site = (fn & ~1u) + 0x12E;
	const uint8_t stock[4] = { 0xA0, 0x23, 0x5B, 0x04 };
	if (sceClibMemcmp((void *)site, stock, sizeof(stock)) != 0) {
		l_error("[texbudget] bytes at %p are not `movs #0xA0; lsls #17` - not patched",
		        (void *)site);
		return;
	}
	const uint8_t patched[4] = { (uint8_t)TEXTURE_BUDGET_MB, 0x23, 0x1B, 0x05 };
	kuKernelCpuUnrestrictedMemcpy((void *)site, patched, sizeof(patched));
	l_info("[texbudget] TextureManager_v2 budget 20 MB -> %d MB", TEXTURE_BUDGET_MB);
}

void so_patch(void) {
	kuser_patch();
	softfloat_vfp_install();
	http_patch();
	expansion_patch();
	cxx_exception_patch();
	mapstate_tracking_patch();
	dpad_map_movement_patch();
	battleintro_controls_patch();
	battle_column_shift_patch();
	gameplay_buttons_patch();
	battle_mode_noop_patch();
	fmod_errorcheck_patch();
	fmod_audio_pump_patch();
	save_names_install();
	music_rewrite_install();
	event_update_gate_install();
	render_find_install();
	render_material_install();
	render_rbuf_install();
	render_sprdata_install();
	render_sortprep_install();
	ai_board_copy_install();
	ai_core_memo_install();
	cheatsheet_lazy_install();
	palette_info_memo_install();
	sprite_dir_record_install();
	scene_entity_index_install();
	render_dynrend_install();
	sprite_setter_c_install();
	render_command_cache_install();
	sprite_state_cache_install();
	game_log_quiet_install();
	text_overlay_cache_install();
	bmptext_cache_install();
	cheatsheet_grid_gate_install();
	transform_same_gate_install();
	transform_cheap_push_install();
	fast_crc32_install();
	loading_drain_install();
	texture_budget_patch();
}
