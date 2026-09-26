/*
 * Copyright (C) 2025 Volodymyr Atamanenko
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include "reimpl/controls.h"
#include "utils/logger.h"
#include "utils/utils.h"

#include <math.h>
#include <stdbool.h>
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
#define LEFT_STICK_DPAD_DEADZONE 40

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

static void poll_virtual_cursor(uint32_t claimed_buttons);
static void poll_battle_pause_tap(uint32_t claimed_buttons);

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
            controls_handler_touch(small_id, x, y, CONTROLS_ACTION_DOWN);
        } else {
            controls_handler_touch(small_id, x, y, CONTROLS_ACTION_MOVE);
        }
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

static void poll_virtual_cursor(uint32_t claimed_buttons) {
    if (map_dpad_is_active() || battle_dpad_is_active()) {
        if (cursor_click_active) {
            controls_handler_touch(CURSOR_TOUCH_ID, cursor_x, cursor_y, CONTROLS_ACTION_UP);
            cursor_click_active = 0;
        }
        return;
    }

    uint32_t cursor_pressed = pressed_buttons & ~claimed_buttons;

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

    if ((pressed_buttons & SCE_CTRL_CIRCLE) && cursor_click_active) {
        controls_handler_touch(CURSOR_TOUCH_ID, cursor_x, cursor_y, CONTROLS_ACTION_UP);
        cursor_click_active = 0;
    }

    float dx = 0.0f, dy = 0.0f;
    if (current_buttons & SCE_CTRL_LEFT)  dx -= 1.0f;
    if (current_buttons & SCE_CTRL_RIGHT) dx += 1.0f;
    if (current_buttons & SCE_CTRL_UP)    dy -= 1.0f;
    if (current_buttons & SCE_CTRL_DOWN)  dy += 1.0f;
    if (dx != 0.0f && dy != 0.0f) {
        dx *= CURSOR_DIAGONAL_SCALE;
        dy *= CURSOR_DIAGONAL_SCALE;
    }

    float speed = CURSOR_SPEED_PX_PER_SEC;
    if (current_buttons & SCE_CTRL_L1) speed *= CURSOR_PRECISION_MULTIPLIER;

    float old_x = cursor_x, old_y = cursor_y;
    cursor_x += dx * speed * dt;
    cursor_y += dy * speed * dt;
    if (cursor_x < 0.0f) cursor_x = 0.0f;
    if (cursor_x > CURSOR_SCREEN_W) cursor_x = CURSOR_SCREEN_W;
    if (cursor_y < 0.0f) cursor_y = 0.0f;
    if (cursor_y > CURSOR_SCREEN_H) cursor_y = CURSOR_SCREEN_H;

    int click_pressed  = (cursor_pressed   & SCE_CTRL_R1) != 0;
    int click_released = (released_buttons & SCE_CTRL_R1) != 0;
    int click_held     = (current_buttons  & SCE_CTRL_R1) != 0;

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

static void poll_battle_pause_tap(uint32_t claimed_buttons) {
    if (pause_tap_active) {
        if ((released_buttons & SCE_CTRL_L1) || !battle_dpad_is_active()) {
            controls_handler_touch(CURSOR_TOUCH_ID, BATTLE_PAUSE_TAP_X, BATTLE_PAUSE_TAP_Y,
                                   CONTROLS_ACTION_UP);
            pause_tap_active = 0;
        }
        return;
    }
    if ((pressed_buttons & ~claimed_buttons & SCE_CTRL_L1) &&
        battle_dpad_is_active() && touch.reportNum == 0) {
        pause_tap_active = 1;
        controls_handler_touch(CURSOR_TOUCH_ID, BATTLE_PAUSE_TAP_X, BATTLE_PAUSE_TAP_Y,
                               CONTROLS_ACTION_DOWN);
    }
}

typedef int (*ButtonActionFn)(int param);

typedef struct {
    uint32_t sce_button;
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
    { SCE_CTRL_UP,       action_map_move, 0 },
    { SCE_CTRL_DOWN,     action_map_move, 1 },
    { SCE_CTRL_LEFT,     action_map_move, 2 },
    { SCE_CTRL_RIGHT,    action_map_move, 3 },
    { SCE_CTRL_CROSS,    action_map_interact, 0 },
    { SCE_CTRL_L1,       action_map_quest_menu, 0 },
    { SCE_CTRL_R1,       action_map_pause_menu, 0 },

    { SCE_CTRL_CROSS,    action_dialogue_confirm, 0 },
    { SCE_CTRL_CIRCLE,   action_dialogue_cancel, 0 },
    { SCE_CTRL_CROSS,    action_prompt_answer, 1 },
    { SCE_CTRL_CIRCLE,   action_prompt_answer, 0 },
    { SCE_CTRL_LEFT,     action_dialogue_choice_horizontal, 1 },
    { SCE_CTRL_RIGHT,    action_dialogue_choice_horizontal, 0 },
    { SCE_CTRL_UP,       action_dialogue_choice_vertical, -1 },
    { SCE_CTRL_DOWN,     action_dialogue_choice_vertical, 1 },

    { SCE_CTRL_CROSS,    action_screen_close, 1 },
    { SCE_CTRL_CIRCLE,   action_screen_close, 0 },
    { SCE_CTRL_LEFT,     action_dwelling_change_amount, -1 },
    { SCE_CTRL_RIGHT,    action_dwelling_change_amount, 1 },

    { SCE_CTRL_CROSS,    action_bookend_next, 0 },
    { SCE_CTRL_CIRCLE,   action_bookend_next, 0 },

    { SCE_CTRL_CROSS,    action_advdeath_select, 1 },
    { SCE_CTRL_CIRCLE,   action_advdeath_select, 0 },

    { SCE_CTRL_CROSS,    action_battleintro_select, 0 },
    { SCE_CTRL_CIRCLE,   action_battleintro_select, 1 },
    { SCE_CTRL_SQUARE,   action_battleintro_select, 2 },

    { SCE_CTRL_LEFT,     action_battle_column_move, -1, 1 },
    { SCE_CTRL_RIGHT,    action_battle_column_move, 1, 1 },
    { SCE_CTRL_CROSS,    action_battle_column_click, 0 },
    { SCE_CTRL_CIRCLE,   action_battle_column_cancel, 0 },
    { SCE_CTRL_R1,       action_battle_zoom_toggle, 0 },
    { SCE_CTRL_SQUARE,   action_battle_cast_spell, 0 },
    { SCE_CTRL_UP,       action_battle_row_move, 1, 1 },
    { SCE_CTRL_DOWN,     action_battle_row_move, -1, 1 },
    { SCE_CTRL_TRIANGLE, action_battle_kill, 0 },
    { SCE_CTRL_SELECT,   action_battle_reinforcements, 0 },
    { SCE_CTRL_START,    action_battle_end_turn, 0 },
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
                s_eligible |= button_actions[i].sce_button;
            }
        }
        s_eligible_built = 1;
    }

    static uint32_t s_button = 0;
    static uint64_t s_held_since_us = 0;
    static uint64_t s_last_fire_us = 0;

    uint64_t now_us = sceKernelGetProcessTimeWide();
    uint32_t held  = current_buttons & s_eligible;
    uint32_t fresh = pressed_buttons & s_eligible;
    if (fresh || !(held & s_button)) {
        uint32_t pick = fresh ? fresh : held;
        s_button = pick & (~pick + 1u);
        s_held_since_us = now_us;
        s_last_fire_us = 0;
    }

    if (!s_button || now_us - s_held_since_us < REPEAT_DELAY_US) {
        return 0;
    }
    if (s_last_fire_us && now_us - s_last_fire_us < REPEAT_RATE_US) {
        return 0;
    }
    s_last_fire_us = now_us;
    return s_button;
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

    {
        static uint32_t old_stick_dpad = 0;
        int stick_dx = (int)pad.lx - 128;
        int stick_dy = (int)pad.ly - 128;
        uint32_t stick_dpad = 0;
        if (stick_dx < -LEFT_STICK_DPAD_DEADZONE) stick_dpad |= SCE_CTRL_LEFT;
        if (stick_dx >  LEFT_STICK_DPAD_DEADZONE) stick_dpad |= SCE_CTRL_RIGHT;
        if (stick_dy < -LEFT_STICK_DPAD_DEADZONE) stick_dpad |= SCE_CTRL_UP;
        if (stick_dy >  LEFT_STICK_DPAD_DEADZONE) stick_dpad |= SCE_CTRL_DOWN;

        current_buttons  |= stick_dpad;
        pressed_buttons  |= stick_dpad & ~old_stick_dpad;
        released_buttons |= ~stick_dpad & old_stick_dpad;
        old_stick_dpad = stick_dpad;
    }

    for (int i = 0; i < sizeof(mapping) / sizeof(ButtonMapping); i++) {
        if (pressed_buttons & mapping[i].sce_button) {
            controls_handler_key(mapping[i].android_button, CONTROLS_ACTION_DOWN);
        }
        if (released_buttons & mapping[i].sce_button) {
            controls_handler_key(mapping[i].android_button, CONTROLS_ACTION_UP);
        }
    }

    uint32_t repeat_buttons = poll_repeat_edges();

    uint32_t claimed_buttons = 0;
    for (int i = 0; i < NUM_BUTTON_ACTIONS; i++) {
        uint32_t button = button_actions[i].sce_button;
        uint32_t edges = pressed_buttons |
                         (button_actions[i].repeats ? repeat_buttons : 0);
        if (!(edges & button) || (claimed_buttons & button)) {
            continue;
        }
        if (button_actions[i].fn(button_actions[i].param)) {
            claimed_buttons |= button;
        }
    }

    poll_virtual_cursor(claimed_buttons);

    poll_battle_pause_tap(claimed_buttons);

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
