#include "utils/init.h"
#include "utils/utils.h"
#include "utils/glutil.h"
#include "utils/dialog.h"
#include "utils/watchdog.h"
#include "utils/logger.h"
#include "utils/build_id.h"
#include "utils/render_split.h"
#include "utils/render_path_profile.h"
#include "utils/event_update_gate.h"
#include "utils/render_command_cache.h"
#include "utils/sprite_state_cache.h"
#include "utils/bmptext_cache.h"
#include "utils/cheatsheet_grid_gate.h"
#include "utils/transform_same_gate.h"
#include "utils/transform_cheap_push.h"
#include "utils/text_overlay_cache.h"
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
#include <vitasdk.h>

#include <stdlib.h>
#include <time.h>
#include <pthread.h>

#include <psp2/kernel/threadmgr.h>

#include <falso_jni/FalsoJNI.h>
#include <so_util/so_util.h>

#include "reimpl/controls.h"
#include "reimpl/iocontent.h"
#include "video.h"
#include "osd.h"

int _newlib_heap_size_user = 256 * 1024 * 1024;

so_module so_mod;

pthread_mutex_t g_engine_call_mutex = PTHREAD_MUTEX_INITIALIZER;

#ifdef PERF_COUNTER_ENABLED
extern volatile uint32_t g_poll_iters;
extern volatile uint32_t g_poll_stage;
#endif

typedef void (*so_setup_core_java_fn)(JNIEnv *env, jclass clazz, void *unused1, uint32_t unused2);
typedef void (*so_create_application_fn)(void);
typedef void (*so_initialise_fn)(JNIEnv *env, jclass clazz, void *unused);
typedef void (*so_frame_begin_fn)(JNIEnv *env, jclass clazz, jfloat deltaTime, jlong timestamp);

typedef void (*so_touch_fn)(JNIEnv *env, jobject thiz, jint id, jfloat x, jfloat y);

#define MOFLOW_CORE_SYM(name) \
    "Java_com_taggames_moflow_nativeinterface_CCoreNativeInterface_" name
#define MOFLOW_TOUCH_SYM(name) \
    "Java_com_taggames_moflow_nativeinterface_CTouchInputNativeInterface_" name

static so_touch_fn TouchDown, TouchUp, TouchMoved;

int main() {
    build_id_log();

    setenv("TZ", "UTC0", 1);
    tzset();

    soloader_init_all();
    log_free_memory("after soloader_init_all");

    iocontent_pack_start();

    watchdog_start();

    gl_init();
    log_free_memory("after gl_init");

    so_setup_core_java_fn SetupCoreJavaNativeInterface =
        (so_setup_core_java_fn)so_symbol(&so_mod, MOFLOW_CORE_SYM("SetupCoreJavaNativeInterface"));
    so_create_application_fn CreateApplication =
        (so_create_application_fn)so_symbol(&so_mod, MOFLOW_CORE_SYM("CreateApplication"));
    so_initialise_fn Initialise =
        (so_initialise_fn)so_symbol(&so_mod, MOFLOW_CORE_SYM("Initialise"));
    so_frame_begin_fn FrameBegin =
        (so_frame_begin_fn)so_symbol(&so_mod, MOFLOW_CORE_SYM("FrameBegin"));

    if (!SetupCoreJavaNativeInterface || !CreateApplication || !Initialise || !FrameBegin) {
        fatal_error("Missing CCoreNativeInterface symbol(s): SetupCoreJavaNativeInterface=%p CreateApplication=%p Initialise=%p FrameBegin=%p",
                    SetupCoreJavaNativeInterface, CreateApplication, Initialise, FrameBegin);
    }

    TouchDown  = (so_touch_fn)so_symbol(&so_mod, MOFLOW_TOUCH_SYM("TouchDown"));
    TouchUp    = (so_touch_fn)so_symbol(&so_mod, MOFLOW_TOUCH_SYM("TouchUp"));
    TouchMoved = (so_touch_fn)so_symbol(&so_mod, MOFLOW_TOUCH_SYM("TouchMoved"));

    if (!TouchDown || !TouchUp || !TouchMoved) {
        fatal_error("Missing CTouchInputNativeInterface symbol(s): TouchDown=%p TouchUp=%p TouchMoved=%p",
                    TouchDown, TouchUp, TouchMoved);
    }

    controls_init();

    SetupCoreJavaNativeInterface(&jni, NULL, NULL, 0);
    CreateApplication();
    Initialise(&jni, NULL, NULL);

#ifdef PERF_COUNTER_ENABLED
    uint32_t frame_loop_iters = 0;
    uint64_t frame_loop_last_us = sceKernelGetProcessTimeWide();
#endif
    uint64_t frame_dt_last_us = sceKernelGetProcessTimeWide();

    while (1) {
        uint64_t now_us = sceKernelGetProcessTimeWide();
        float delta_time = (float)((double)(now_us - frame_dt_last_us) / 1000000.0);
        if (delta_time > 0.5f) delta_time = 0.5f;
        frame_dt_last_us = now_us;

        pthread_mutex_lock(&g_engine_call_mutex);
        FrameBegin(&jni, NULL, delta_time, (jlong)now_us);
        pthread_mutex_unlock(&g_engine_call_mutex);
        video_frame();
        osd_frame();
        gl_swap();

#ifdef PERF_COUNTER_ENABLED
        frame_loop_iters++;
        if ((frame_loop_iters % 300) == 0) {
            uint64_t diag_now_us = sceKernelGetProcessTimeWide();
            uint64_t elapsed_us = diag_now_us - frame_loop_last_us;
            l_perf("[frameloop-diag] 300 iters in %llu us (%.1f iters/sec) "
                   "poll_iters=%u poll_stage=%u",
                   (unsigned long long)elapsed_us,
                   elapsed_us ? 300.0 * 1000000.0 / (double)elapsed_us : 0.0,
                   g_poll_iters, g_poll_stage);
            frame_loop_last_us = diag_now_us;
        }
#endif
    }

    sceKernelExitDeleteThread(0);
}

void controls_handler_key(int32_t keycode, ControlsAction action) {
}

extern pthread_mutex_t g_engine_call_mutex;

void controls_handler_touch(int32_t id, float x, float y, ControlsAction action) {
    so_touch_fn fn = (action == CONTROLS_ACTION_DOWN) ? TouchDown :
                      (action == CONTROLS_ACTION_UP)   ? TouchUp  : TouchMoved;
    if (!fn) {
        return;
    }
    pthread_mutex_lock(&g_engine_call_mutex);
    fn(&jni, NULL, id, x, y);
    pthread_mutex_unlock(&g_engine_call_mutex);
}

void controls_handler_analog(ControlsStickId which, float x, float y, ControlsAction action) {
}
