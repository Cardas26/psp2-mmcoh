/*
 * Copyright (C) 2025 Volodymyr Atamanenko
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include "reimpl/controls.h"
#include "osd.h"
#include "utils/campaign_tap.h"
#include "utils/logger.h"
#include "utils/utils.h"

#include <keymap/keymap.h>

#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <psp2/ctrl.h>
#include <psp2/motion.h>
#include <psp2/touch.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>

#if defined(PERF_COUNTER_ENABLED) || defined(HANG_WATCHDOG)
volatile uint32_t g_poll_iters = 0;
volatile uint32_t g_poll_stage = 0;
#define POLL_ITER()   (g_poll_iters++)
#define POLL_STAGE(n) (g_poll_stage = (n))
#else
#define POLL_ITER()   ((void)0)
#define POLL_STAGE(n) ((void)0)
#endif

#define LEFT_ANALOG_DEADZONE  0.16f
#define RIGHT_ANALOG_DEADZONE 0.16f
#define STICK_DIR_DEADZONE 40

void coord_normalize(float * x, float * y, float deadzone) {
    float magnitude = sqrtf((*x * *x) + (*y * *y));
    if (magnitude < deadzone) {
        *x = 0;
        *y = 0;
        return;
    }

    *x = *x / magnitude;
    *y = *y / magnitude;

    float multiplier = ((magnitude - deadzone) / (1 - deadzone));
    *x = *x * multiplier;
    *y = *y * multiplier;
}

void poll_touch();
void poll_pad();
void poll_accel();

void poll_stick(ControlsStickId which, float raw_x, float raw_y, float * readings_x, float * readings_y, float deadzone);

static void poll_virtual_cursor(uint32_t claimed);
static void poll_battle_pause_tap(uint32_t claimed);
static void keymap_load(void);

bool map_dpad_is_active(void);

int map_dpad_move(int direction);

int battleintro_select(int which);

bool battle_dpad_is_active(void);
int battle_column_move(int delta);
int battle_column_click(void);
int battle_column_cancel(void);
int battle_zoom_toggle(void);
int battle_cast_spell(void);
int battle_row_move(int delta);
int battle_kill(void);
int battle_reinforcements(void);
int battle_end_turn(void);
int map_interact(void);
int dialogue_confirm(void);
int dialogue_cancel(void);
int prompt_answer(int yes);
int dialogue_choice_horizontal(int to_yes);
int dialogue_choice_vertical(int delta);
int screen_close(int accept);
int bookend_next(void);
int advdeath_select(int resume);
int dwelling_change_amount(int delta);
int map_quest_menu(void);
int map_pause_menu(void);

void controls_poll() {
    POLL_STAGE(0);
    poll_touch();
    poll_pad();
    POLL_STAGE(5);
}

static int controls_poll_thread(SceSize args, void * argp) {
    while (1) {
        controls_poll();
        POLL_ITER();
        sceKernelDelayThread(16666);
    }
    return 0;
}

#define CONTROLS_POLL_STACK_SIZE (512 * 1024)

#define CONTROLS_POLL_PRIORITY 0x10000100

void controls_init() {
    int ret = sceCtrlSetSamplingModeExt(SCE_CTRL_MODE_ANALOG_WIDE);
    if (ret < 0)
        l_error("controls: sceCtrlSetSamplingModeExt failed: 0x%08X", ret);

    ret = sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT, 1);
    if (ret < 0)
        l_error("controls: sceTouchSetSamplingState failed: 0x%08X", ret);

    ret = sceMotionStartSampling();
    if (ret < 0)
        l_error("controls: sceMotionStartSampling failed: 0x%08X", ret);

    keymap_load();

    SceUID t = sceKernelCreateThread("controls_poll", controls_poll_thread,
                                     CONTROLS_POLL_PRIORITY,
                                     CONTROLS_POLL_STACK_SIZE, 0, 0, NULL);
    if (t < 0) {
        l_fatal("controls: sceKernelCreateThread failed: 0x%08X - NO INPUT", t);
        return;
    }

    ret = sceKernelStartThread(t, 0, NULL);
    if (ret < 0) {
        l_fatal("controls: sceKernelStartThread failed: 0x%08X - NO INPUT", ret);
        sceKernelDeleteThread(t);
    }
}

static SceTouchData touch;
static SceTouchData touch_old;

static float g_last_touch_x = 480.f, g_last_touch_y = 400.f;

#define TOUCH_ID_SLOTS 10
#define TOUCH_ID_REAL_SLOTS 9
static int touch_slot_raw_id[TOUCH_ID_SLOTS] = {
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1
};

static int touch_id_lookup(int raw_id) {
    for (int i = 0; i < TOUCH_ID_REAL_SLOTS; i++) {
        if (touch_slot_raw_id[i] == raw_id) {
            return i;
        }
    }
    return -1;
}

static int touch_id_alloc(int raw_id) {
    int existing = touch_id_lookup(raw_id);
    if (existing >= 0) {
        return existing;
    }
    for (int i = 0; i < TOUCH_ID_REAL_SLOTS; i++) {
        if (touch_slot_raw_id[i] == -1) {
            touch_slot_raw_id[i] = raw_id;
            return i;
        }
    }
    return -1;
}

static void touch_id_free(int raw_id) {
    int slot = touch_id_lookup(raw_id);
    if (slot >= 0) {
        touch_slot_raw_id[slot] = -1;
    }
}

#define TOUCH_PANEL_W 1920.0f
#define TOUCH_PANEL_H 1088.0f

static bool touch_pinned[TOUCH_ID_SLOTS];
static float touch_pin_x[TOUCH_ID_SLOTS];
static float touch_pin_y[TOUCH_ID_SLOTS];

void poll_touch() {
    POLL_STAGE(1);
    sceTouchPeek(SCE_TOUCH_PORT_FRONT, &touch, 1);

    for (int i = 0; i < touch.reportNum; i++) {
        POLL_STAGE(2);
        float x = (float) touch.report[i].x * (float)SCREEN_NATIVE_W / TOUCH_PANEL_W;
        float y = (float) touch.report[i].y * (float)SCREEN_NATIVE_H / TOUCH_PANEL_H;
        g_last_touch_x = x;
        g_last_touch_y = y;

        int finger_down = 0;

        if (touch_old.reportNum > 0) {
            for (int j = 0; j < touch_old.reportNum; j++) {
                if (touch.report[i].id == touch_old.report[j].id) {
                    finger_down = 1;
                    break;
                }
            }
        }

        int small_id = touch_id_alloc(touch.report[i].id);
        if (small_id < 0) {
            l_error("[touch] no free slot for raw_id=%d, event dropped",
                    touch.report[i].id);
            continue;
        }

        if (!finger_down) {
            l_debug("[touch-down-diag] DOWN id=%d raw_id=%d pos=(%.1f,%.1f)",
                small_id, touch.report[i].id, x, y);
            touch_pinned[small_id] = campaign_tap_pin(x, y,
                &touch_pin_x[small_id], &touch_pin_y[small_id]);
        }
        if (touch_pinned[small_id]) {
            x = touch_pin_x[small_id];
            y = touch_pin_y[small_id];
        }
        controls_handler_touch(small_id, x, y,
            finger_down ? CONTROLS_ACTION_MOVE : CONTROLS_ACTION_DOWN);
    }

    for (int i = 0; i < touch_old.reportNum; i++) {
        int finger_up = 1;

        for (int j = 0; j < touch.reportNum; j++) {
            if (touch.report[j].id == touch_old.report[i].id ) {
                finger_up = 0;
                break;
            }
        }

        if (finger_up == 1) {
            float x = (float) touch_old.report[i].x * (float)SCREEN_NATIVE_W / TOUCH_PANEL_W;
            float y = (float) touch_old.report[i].y * (float)SCREEN_NATIVE_H / TOUCH_PANEL_H;

            int small_id = touch_id_lookup(touch_old.report[i].id);
            if (small_id < 0) {
                continue;
            }
            touch_id_free(touch_old.report[i].id);

            l_debug("[touch-down-diag] UP id=%d raw_id=%d pos=(%.1f,%.1f)",
                small_id, touch_old.report[i].id, x, y);
            if (touch_pinned[small_id]) {
                x = touch_pin_x[small_id];
                y = touch_pin_y[small_id];
                touch_pinned[small_id] = false;
            }
            controls_handler_touch(small_id, x, y, CONTROLS_ACTION_UP);
        }
    }

    sceClibMemcpy(&touch_old, &touch, sizeof(touch));
}

static ButtonMapping mapping[] = {
        { SCE_CTRL_UP,        AKEYCODE_DPAD_UP },
        { SCE_CTRL_DOWN,      AKEYCODE_DPAD_DOWN },
        { SCE_CTRL_LEFT,      AKEYCODE_DPAD_LEFT },
        { SCE_CTRL_RIGHT,     AKEYCODE_DPAD_RIGHT },
        { SCE_CTRL_CROSS,     AKEYCODE_BUTTON_A },
        { SCE_CTRL_CIRCLE,    AKEYCODE_BUTTON_B },
        { SCE_CTRL_SQUARE,    AKEYCODE_BUTTON_X },
        { SCE_CTRL_TRIANGLE,  AKEYCODE_BUTTON_Y },
        { SCE_CTRL_L1,        AKEYCODE_BUTTON_L1 },
        { SCE_CTRL_R1,        AKEYCODE_BUTTON_R1 },
        { SCE_CTRL_START,     AKEYCODE_BUTTON_START },
        { SCE_CTRL_SELECT,    AKEYCODE_BUTTON_SELECT },
};

static uint32_t old_buttons = 0, current_buttons = 0, pressed_buttons = 0, released_buttons = 0;

enum {
    CTX_MAP    = 1,
    CTX_BATTLE = 2,
    CTX_MENUS  = 4,
    CTX_PLAY   = CTX_MAP | CTX_BATTLE | CTX_MENUS,
};
enum {
    ACT_UP, ACT_DOWN, ACT_LEFT, ACT_RIGHT, ACT_CONFIRM, ACT_BACK,
    ACT_QUESTLOG, ACT_MAPMENU, ACT_BATTLEMENU, ACT_ZOOM, ACT_SPELL,
    ACT_REMOVE, ACT_REINFORCE, ACT_ENDTURN, ACT_SETUP, ACT_CURSORTAP,
    ACT_CURSORSLOW, ACT_N
};
#define ACT(a) (1u << ACT_##a)

static const struct km_act pad_acts[ACT_N] = {
    [ACT_UP]         = { "Up", "Up, LStickUp",
                         "map: step up; a list: up; battle: the row up; elsewhere: the cursor",
                         KM_BUTTONS, 2, CTX_PLAY },
    [ACT_DOWN]       = { "Down", "Down, LStickDown",
                         "map: step down; a list: down; battle: the row down; elsewhere: the cursor",
                         KM_BUTTONS, 2, CTX_PLAY },
    [ACT_LEFT]       = { "Left", "Left, LStickLeft",
                         "map: step left; Yes/No: Yes; dwelling: one fewer; battle: the column left; elsewhere: the cursor",
                         KM_BUTTONS, 2, CTX_PLAY },
    [ACT_RIGHT]      = { "Right", "Right, LStickRight",
                         "map: step right; Yes/No: No; dwelling: one more; battle: the column right; elsewhere: the cursor",
                         KM_BUTTONS, 2, CTX_PLAY },
    [ACT_CONFIRM]    = { "Confirm", "Cross",
                         "map: use the hero's tile; next, choose, Yes, OK, close, Buy, Continue; VS: Battle; battle: take or drop a column",
                         KM_BUTTONS, 1, CTX_PLAY },
    [ACT_BACK]       = { "Back", "Circle",
                         "cancel, No, close, back, Exit game; VS: Flee; battle: put the column back; lifts a stuck cursor tap",
                         KM_BUTTONS, 1, CTX_PLAY },
    [ACT_QUESTLOG]   = { "QuestLog", "L", "map: the quest log", KM_BUTTONS, 0, CTX_MAP },
    [ACT_MAPMENU]    = { "MapMenu", "R", "map: the pause menu", KM_BUTTONS, 0, CTX_MAP },
    [ACT_BATTLEMENU] = { "BattleMenu", "L", "battle: the pause menu", KM_BUTTONS, 0, CTX_BATTLE },
    [ACT_ZOOM]       = { "Zoom", "R", "battle: zoom the board in or out", KM_BUTTONS, 0, CTX_BATTLE },
    [ACT_SPELL]      = { "Spell", "Square", "battle: the hero's spell", KM_BUTTONS, 0, CTX_BATTLE },
    [ACT_REMOVE]     = { "Remove", "Triangle", "battle: remove the selected unit", KM_BUTTONS, 0, CTX_BATTLE },
    [ACT_REINFORCE]  = { "Reinforce", "Select", "battle: call reinforcements", KM_BUTTONS, 0, CTX_BATTLE },
    [ACT_ENDTURN]    = { "EndTurn", "Start", "battle: end the turn", KM_BUTTONS, 0, CTX_BATTLE },
    [ACT_SETUP]      = { "Setup", "Square", "VS screen: set up units", KM_BUTTONS, 0, CTX_MENUS },
    [ACT_CURSORTAP]  = { "CursorTap", "R",
                         "other screens: tap at the cursor, drag while held", KM_BUTTONS, 0, CTX_MENUS },
    [ACT_CURSORSLOW] = { "CursorSlow", "L",
                         "other screens: the cursor moves slowly while held", KM_BUTTONS, 0, CTX_MENUS },
};

static struct km_map g_km;
static uint32_t act_shares[ACT_N];
static uint32_t acts_held = 0, acts_pressed = 0, acts_released = 0;
static volatile int skip_held = 0;

int controls_skip_held(void) {
    return skip_held;
}

static void keymap_log(const char *line) {
    (void)line;
    l_info("%s", line);
}

static void keymap_load(void) {
    g_km.path = DATA_PATH "controls.ini";
    g_km.aside = DATA_PATH "controls-unreadable.ini";
    g_km.title = "Clash of Heroes";
    g_km.header =
        "; Touch is not a button: it always taps. Any button or a touch skips a cutscene.\n"
        "; A button can do one thing on each screen: the map, a battle, and every other screen (menus,\n"
        ";   conversations, the VS screen), where the directions move a cursor. Two actions on one button\n"
        ";   and one screen are a mistake, named on the screen at start.\n";
    g_km.acts = pad_acts;
    g_km.n_acts = ACT_N;
    g_km.avail = (1u << KM_N) - 1;
    g_km.log = keymap_log;
    km_load(&g_km);
    char note[96];
    for (int i = 0; km_note(&g_km, i, note, sizeof(note)); i++)
        osd_queue(note, 4000);

    for (int a = 0; a < ACT_N; a++) {
        const struct km_bind *x = &g_km.bind[a];
        act_shares[a] = 1u << a;
        for (int b = 0; b < ACT_N; b++) {
            const struct km_bind *y = &g_km.bind[b];
            for (int i = 0; i < x->n; i++)
                for (int j = 0; j < y->n; j++)
                    if (x->mod[i] == y->mod[j] && x->btn[i] == y->btn[j])
                        act_shares[a] |= 1u << b;
        }
    }
}

static uint32_t pad_on(const SceCtrlData *pad) {
    static const struct { uint32_t sce; uint8_t km; } bits[] = {
        { SCE_CTRL_UP, KM_UP }, { SCE_CTRL_DOWN, KM_DOWN },
        { SCE_CTRL_LEFT, KM_LEFT }, { SCE_CTRL_RIGHT, KM_RIGHT },
        { SCE_CTRL_CROSS, KM_CROSS }, { SCE_CTRL_CIRCLE, KM_CIRCLE },
        { SCE_CTRL_SQUARE, KM_SQUARE }, { SCE_CTRL_TRIANGLE, KM_TRI },
        { SCE_CTRL_L1 | SCE_CTRL_LTRIGGER, KM_L }, { SCE_CTRL_R1 | SCE_CTRL_RTRIGGER, KM_R },
        { SCE_CTRL_START, KM_START }, { SCE_CTRL_SELECT, KM_SELECT },
    };
    uint32_t on = 0;
    for (size_t i = 0; i < sizeof(bits) / sizeof(bits[0]); i++)
        if (pad->buttons & bits[i].sce)
            on |= 1u << bits[i].km;
    int lx = (int)pad->lx - 128, ly = (int)pad->ly - 128;
    int rx = (int)pad->rx - 128, ry = (int)pad->ry - 128;
    if (ly < -STICK_DIR_DEADZONE) on |= 1u << KM_LS_UP;
    if (ly >  STICK_DIR_DEADZONE) on |= 1u << KM_LS_DOWN;
    if (lx < -STICK_DIR_DEADZONE) on |= 1u << KM_LS_LEFT;
    if (lx >  STICK_DIR_DEADZONE) on |= 1u << KM_LS_RIGHT;
    if (ry < -STICK_DIR_DEADZONE) on |= 1u << KM_RS_UP;
    if (ry >  STICK_DIR_DEADZONE) on |= 1u << KM_RS_DOWN;
    if (rx < -STICK_DIR_DEADZONE) on |= 1u << KM_RS_LEFT;
    if (rx >  STICK_DIR_DEADZONE) on |= 1u << KM_RS_RIGHT;
    return on;
}

static float analog_lx[3] = { 0 };
static float analog_ly[3] = { 0 };
static float analog_rx[3] = { 0 };
static float analog_ry[3] = { 0 };

#define CURSOR_TOUCH_ID (TOUCH_ID_SLOTS - 1)
#define CURSOR_SPEED_PX_PER_SEC 480.0f
#define CURSOR_PRECISION_MULTIPLIER 0.35f
#define CURSOR_MOVE_EPSILON 0.5f
#define CURSOR_SCREEN_W ((float)SCREEN_NATIVE_W)
#define CURSOR_SCREEN_H ((float)SCREEN_NATIVE_H)
#define CURSOR_DIAGONAL_SCALE 0.70710678f

static float cursor_x = CURSOR_SCREEN_W / 2.0f;
static float cursor_y = CURSOR_SCREEN_H / 2.0f;
static int cursor_click_active = 0;
static uint64_t cursor_last_us = 0;

static void poll_virtual_cursor(uint32_t claimed) {
    if (map_dpad_is_active() || battle_dpad_is_active()) {
        if (cursor_click_active) {
            controls_handler_touch(CURSOR_TOUCH_ID, cursor_x, cursor_y, CONTROLS_ACTION_UP);
            cursor_click_active = 0;
        }
        return;
    }

    uint32_t cursor_pressed = acts_pressed & ~claimed;

    uint64_t now_us = sceKernelGetProcessTimeWide();
    float dt = cursor_last_us ? (float)((double)(now_us - cursor_last_us) / 1000000.0) : 0.0f;
    if (dt > 0.25f) dt = 0.25f;
    cursor_last_us = now_us;

    if (touch.reportNum > 0) {
        if (cursor_click_active) {
            controls_handler_touch(CURSOR_TOUCH_ID, cursor_x, cursor_y, CONTROLS_ACTION_UP);
            cursor_click_active = 0;
        }
        return;
    }

    if ((acts_pressed & ACT(BACK)) && cursor_click_active) {
        controls_handler_touch(CURSOR_TOUCH_ID, cursor_x, cursor_y, CONTROLS_ACTION_UP);
        cursor_click_active = 0;
    }

    float dx = 0.0f, dy = 0.0f;
    if (acts_held & ACT(LEFT))  dx -= 1.0f;
    if (acts_held & ACT(RIGHT)) dx += 1.0f;
    if (acts_held & ACT(UP))    dy -= 1.0f;
    if (acts_held & ACT(DOWN))  dy += 1.0f;
    if (dx != 0.0f && dy != 0.0f) {
        dx *= CURSOR_DIAGONAL_SCALE;
        dy *= CURSOR_DIAGONAL_SCALE;
    }

    float speed = CURSOR_SPEED_PX_PER_SEC;
    if (acts_held & ACT(CURSORSLOW)) speed *= CURSOR_PRECISION_MULTIPLIER;

    float old_x = cursor_x, old_y = cursor_y;
    cursor_x += dx * speed * dt;
    cursor_y += dy * speed * dt;
    if (cursor_x < 0.0f) cursor_x = 0.0f;
    if (cursor_x > CURSOR_SCREEN_W) cursor_x = CURSOR_SCREEN_W;
    if (cursor_y < 0.0f) cursor_y = 0.0f;
    if (cursor_y > CURSOR_SCREEN_H) cursor_y = CURSOR_SCREEN_H;

    int click_pressed  = (cursor_pressed & ACT(CURSORTAP)) != 0;
    int click_released = (acts_released  & ACT(CURSORTAP)) != 0;
    int click_held     = (acts_held      & ACT(CURSORTAP)) != 0;

    if (click_released && cursor_click_active) {
        controls_handler_touch(CURSOR_TOUCH_ID, old_x, old_y, CONTROLS_ACTION_UP);
        cursor_click_active = 0;
    } else if (click_pressed && !cursor_click_active) {
        cursor_click_active = 1;
        controls_handler_touch(CURSOR_TOUCH_ID, cursor_x, cursor_y, CONTROLS_ACTION_DOWN);
    } else if (click_held && cursor_click_active) {
        float moved_x = cursor_x - old_x, moved_y = cursor_y - old_y;
        if (fabsf(moved_x) > CURSOR_MOVE_EPSILON || fabsf(moved_y) > CURSOR_MOVE_EPSILON) {
            controls_handler_touch(CURSOR_TOUCH_ID, cursor_x, cursor_y, CONTROLS_ACTION_MOVE);
        }
    }
}

#define BATTLE_PAUSE_TAP_X 28.0f
#define BATTLE_PAUSE_TAP_Y (CURSOR_SCREEN_H / 2.0f)

static int pause_tap_active = 0;

static void poll_battle_pause_tap(uint32_t claimed) {
    if (pause_tap_active) {
        if ((acts_released & ACT(BATTLEMENU)) || !battle_dpad_is_active()) {
            controls_handler_touch(CURSOR_TOUCH_ID, BATTLE_PAUSE_TAP_X, BATTLE_PAUSE_TAP_Y,
                                   CONTROLS_ACTION_UP);
            pause_tap_active = 0;
        }
        return;
    }
    if ((acts_pressed & ~claimed & ACT(BATTLEMENU)) &&
        battle_dpad_is_active() && touch.reportNum == 0) {
        pause_tap_active = 1;
        controls_handler_touch(CURSOR_TOUCH_ID, BATTLE_PAUSE_TAP_X, BATTLE_PAUSE_TAP_Y,
                               CONTROLS_ACTION_DOWN);
    }
}

typedef int (*ButtonActionFn)(int param);

typedef struct {
    int act;
    ButtonActionFn fn;
    int param;
    int repeats;
} ButtonAction;

static int action_map_move(int direction)          { return map_dpad_move(direction); }
static int action_battleintro_select(int which)     { return battleintro_select(which); }
static int action_battle_column_move(int delta)     { return battle_column_move(delta); }
static int action_battle_column_click(int unused)   { (void)unused; return battle_column_click(); }
static int action_battle_column_cancel(int unused)  { (void)unused; return battle_column_cancel(); }
static int action_battle_zoom_toggle(int unused)    { (void)unused; return battle_zoom_toggle(); }
static int action_battle_cast_spell(int unused)     { (void)unused; return battle_cast_spell(); }
static int action_battle_row_move(int direction)    { return battle_row_move(direction); }
static int action_battle_kill(int unused)           { (void)unused; return battle_kill(); }
static int action_battle_reinforcements(int unused) { (void)unused; return battle_reinforcements(); }
static int action_battle_end_turn(int unused)       { (void)unused; return battle_end_turn(); }
static int action_map_interact(int unused)          { (void)unused; return map_interact(); }
static int action_dialogue_confirm(int unused)      { (void)unused; return dialogue_confirm(); }
static int action_dialogue_cancel(int unused)       { (void)unused; return dialogue_cancel(); }
static int action_prompt_answer(int yes)            { return prompt_answer(yes); }
static int action_dialogue_choice_horizontal(int y) { return dialogue_choice_horizontal(y); }
static int action_dialogue_choice_vertical(int d)   { return dialogue_choice_vertical(d); }
static int action_screen_close(int accept)          { return screen_close(accept); }
static int action_dwelling_change_amount(int delta) { return dwelling_change_amount(delta); }
static int action_bookend_next(int unused)          { (void)unused; return bookend_next(); }
static int action_advdeath_select(int resume)       { return advdeath_select(resume); }
static int action_map_quest_menu(int unused)        { (void)unused; return map_quest_menu(); }
static int action_map_pause_menu(int unused)        { (void)unused; return map_pause_menu(); }

static const ButtonAction button_actions[] = {
    { ACT_UP,          action_map_move, 0 },
    { ACT_DOWN,        action_map_move, 1 },
    { ACT_LEFT,        action_map_move, 2 },
    { ACT_RIGHT,       action_map_move, 3 },
    { ACT_CONFIRM,     action_map_interact, 0 },
    { ACT_QUESTLOG,    action_map_quest_menu, 0 },
    { ACT_MAPMENU,     action_map_pause_menu, 0 },

    { ACT_CONFIRM,     action_dialogue_confirm, 0 },
    { ACT_BACK,        action_dialogue_cancel, 0 },
    { ACT_CONFIRM,     action_prompt_answer, 1 },
    { ACT_BACK,        action_prompt_answer, 0 },
    { ACT_LEFT,        action_dialogue_choice_horizontal, 1 },
    { ACT_RIGHT,       action_dialogue_choice_horizontal, 0 },
    { ACT_UP,          action_dialogue_choice_vertical, -1 },
    { ACT_DOWN,        action_dialogue_choice_vertical, 1 },

    { ACT_CONFIRM,     action_screen_close, 1 },
    { ACT_BACK,        action_screen_close, 0 },
    { ACT_LEFT,        action_dwelling_change_amount, -1 },
    { ACT_RIGHT,       action_dwelling_change_amount, 1 },

    { ACT_CONFIRM,     action_bookend_next, 0 },
    { ACT_BACK,        action_bookend_next, 0 },

    { ACT_CONFIRM,     action_advdeath_select, 1 },
    { ACT_BACK,        action_advdeath_select, 0 },

    { ACT_CONFIRM,     action_battleintro_select, 0 },
    { ACT_BACK,        action_battleintro_select, 1 },
    { ACT_SETUP,       action_battleintro_select, 2 },

    { ACT_LEFT,        action_battle_column_move, -1, 1 },
    { ACT_RIGHT,       action_battle_column_move, 1, 1 },
    { ACT_CONFIRM,     action_battle_column_click, 0 },
    { ACT_BACK,        action_battle_column_cancel, 0 },
    { ACT_ZOOM,        action_battle_zoom_toggle, 0 },
    { ACT_SPELL,       action_battle_cast_spell, 0 },
    { ACT_UP,          action_battle_row_move, 1, 1 },
    { ACT_DOWN,        action_battle_row_move, -1, 1 },
    { ACT_REMOVE,      action_battle_kill, 0 },
    { ACT_REINFORCE,   action_battle_reinforcements, 0 },
    { ACT_ENDTURN,     action_battle_end_turn, 0 },
};
#define NUM_BUTTON_ACTIONS (sizeof(button_actions) / sizeof(button_actions[0]))

#define REPEAT_DELAY_US 250000
#define REPEAT_RATE_US   83000

static uint32_t poll_repeat_edges(void) {
    static uint32_t s_eligible = 0;
    static int s_eligible_built = 0;
    if (!s_eligible_built) {
        for (int i = 0; i < NUM_BUTTON_ACTIONS; i++) {
            if (button_actions[i].repeats) {
                s_eligible |= 1u << button_actions[i].act;
            }
        }
        s_eligible_built = 1;
    }

    static uint32_t s_act = 0;
    static uint64_t s_held_since_us = 0;
    static uint64_t s_last_fire_us = 0;

    uint64_t now_us = sceKernelGetProcessTimeWide();
    uint32_t held  = acts_held & s_eligible;
    uint32_t fresh = acts_pressed & s_eligible;
    if (fresh || !(held & s_act)) {
        uint32_t pick = fresh ? fresh : held;
        s_act = pick & (~pick + 1u);
        s_held_since_us = now_us;
        s_last_fire_us = 0;
    }

    if (!s_act || now_us - s_held_since_us < REPEAT_DELAY_US) {
        return 0;
    }
    if (s_last_fire_us && now_us - s_last_fire_us < REPEAT_RATE_US) {
        return 0;
    }
    s_last_fire_us = now_us;
    return s_act;
}

void poll_pad() {
    POLL_STAGE(3);
    SceCtrlData pad;
    sceCtrlPeekBufferPositiveExt2(0, &pad, 1);
    POLL_STAGE(4);

    old_buttons = current_buttons;
    current_buttons = pad.buttons;
    pressed_buttons = current_buttons & ~old_buttons;
    released_buttons = ~current_buttons & old_buttons;

    uint32_t on = pad_on(&pad);

    uint8_t hit[ACT_N], fire[ACT_N];
    km_eval(&g_km, on, hit, fire);
    uint32_t held = 0, landed = 0;
    for (int a = 0; a < ACT_N; a++) {
        if (hit[a]) held |= 1u << a;
        if (fire[a]) landed |= 1u << a;
    }
    acts_released = acts_held & ~held;
    acts_held = held;
    acts_pressed = landed;
    skip_held = (on & ((1u << KM_RS_UP) - 1)) != 0;

    for (int i = 0; i < sizeof(mapping) / sizeof(ButtonMapping); i++) {
        if (pressed_buttons & mapping[i].sce_button) {
            controls_handler_key(mapping[i].android_button, CONTROLS_ACTION_DOWN);
        }
        if (released_buttons & mapping[i].sce_button) {
            controls_handler_key(mapping[i].android_button, CONTROLS_ACTION_UP);
        }
    }

    uint32_t repeat_acts = poll_repeat_edges();

    uint32_t claimed = 0;
    for (int i = 0; i < NUM_BUTTON_ACTIONS; i++) {
        uint32_t act = 1u << button_actions[i].act;
        uint32_t edges = acts_pressed |
                         (button_actions[i].repeats ? repeat_acts : 0);
        if (!(edges & act) || (claimed & act)) {
            continue;
        }
        if (button_actions[i].fn(button_actions[i].param)) {
            claimed |= act_shares[button_actions[i].act];
        }
    }

    poll_virtual_cursor(claimed);

    poll_battle_pause_tap(claimed);

    poll_stick(CONTROLS_STICK_LEFT, (float)pad.lx, (float)pad.ly, analog_lx, analog_ly, LEFT_ANALOG_DEADZONE);
    poll_stick(CONTROLS_STICK_RIGHT, (float)pad.rx, (float)pad.ry, analog_rx, analog_ry, RIGHT_ANALOG_DEADZONE);
}

void poll_stick(ControlsStickId which, float raw_x, float raw_y, float * readings_x, float * readings_y, float deadzone) {
    readings_x[0] = (raw_x - 128.0f) / 128.0f;
    readings_y[0] = (raw_y - 128.0f) / 128.0f;

    coord_normalize(&readings_x[0], &readings_y[0], deadzone);

    if (
        (readings_x[0] == 0.f && readings_y[0] == 0.f) &&
        (readings_x[1] == 0.f && readings_y[1] == 0.f) &&
        (readings_x[2] != 0.f || readings_y[2] != 0.f)
    ) {
        controls_handler_analog(which, readings_x[0], readings_y[0], CONTROLS_ACTION_UP);
    }
    else if (
        (readings_x[0] != 0.f || readings_y[0] != 0.f) &&
        (readings_x[1] == 0.f && readings_y[1] == 0.f) &&
        (readings_x[2] == 0.f && readings_y[2] == 0.f)
    ) {
        controls_handler_analog(which, readings_x[0], readings_y[0], CONTROLS_ACTION_DOWN);
    }
    else {
        controls_handler_analog(which, readings_x[0], readings_y[0], CONTROLS_ACTION_MOVE);
    }

    readings_x[2] = readings_x[1];
    readings_y[2] = readings_y[1];
    readings_x[1] = readings_x[0];
    readings_y[1] = readings_y[0];
}
