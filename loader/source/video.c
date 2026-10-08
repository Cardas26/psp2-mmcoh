#include "video.h"

#include "subtitles.h"
#include "reimpl/controls.h"
#include "reimpl/iopath.h"
#include "utils/logger.h"
#include "utils/utils.h"

#include <falso_jni/FalsoJNI.h>
#include <so_util/so_util.h>

#include <psp2/avplayer.h>
#include <psp2/audioout.h>
#include <psp2/touch.h>
#include <psp2/io/dirent.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/sysmodule.h>
#include <psp2/gxm.h>
#include <vitaGL.h>

#include <malloc.h>
#include <pthread.h>
#include <stdio.h>
#include <strings.h>
#include <stdlib.h>
#include <string.h>

extern so_module so_mod;
extern pthread_mutex_t g_engine_call_mutex;

#define VIDEO_BUFFERS 5

#define VIDEO_FIRST_FRAME_TIMEOUT_US (5 * 1000 * 1000)

#define VIDEO_AUDIO_THREAD_PRIORITY (0x10000100 - 1)

typedef void (*so_video_cb_fn)(JNIEnv *env, jobject thiz);

enum { VIDEO_IDLE, VIDEO_STARTING, VIDEO_PLAYING };

static volatile int g_state = VIDEO_IDLE;
static SceAvPlayerHandle g_player;
static bool g_can_dismiss;
static bool g_has_subtitles;
static uint64_t g_start_us;
static uint64_t g_duration_ms;
static int g_frame_idx;
static uint32_t g_frame_w, g_frame_h;
static uint64_t g_first_frame_us;
static volatile unsigned g_audio_blocks;
static volatile int g_audio_mode = SCE_AUDIO_OUT_MODE_STEREO;

static bool g_saw_release;

static GLuint g_tex[VIDEO_BUFFERS];
static SceGxmTexture *g_gxm_tex[VIDEO_BUFFERS];
static GLuint g_program;
static GLint g_uniform_tex;
static bool g_gl_ready;

static SceUID g_audio_thid = -1;
static volatile int g_audio_run;

static so_video_cb_fn g_Stopped, g_Dismissed, g_UpdateSubtitles;

#define PHYCONT_MEM_ALIGNMENT (1024 * 1024)
#define ALIGN_MEM(x, align) (((x) + ((align) - 1)) & ~((align) - 1))

#define GPU_ALLOC_SLOTS 32
static struct { SceUID blk; void *base; } g_gpu_allocs[GPU_ALLOC_SLOTS];

static void player_event(void *p, int32_t id, int32_t source, void *data) {
    (void)p;
    l_debug("[video] player event %d source %d data %#x after %llu ms", id, source,
            data ? (unsigned)*(int32_t *)data : 0u,
            (unsigned long long)((sceKernelGetProcessTimeWide() - g_start_us) / 1000));
}

static void *alloc_for_cpu(void *p, uint32_t align, uint32_t size) {
    (void)p;
    return memalign(align ? align : 16, size);
}

static void free_for_cpu(void *p, void *ptr) {
    (void)p;
    free(ptr);
}

static unsigned phycont_free_kb(void) {
    SceKernelFreeMemorySizeInfo info;
    info.size = sizeof(info);
    sceKernelGetFreeMemorySize(&info);
    return (unsigned)(info.size_phycont / 1024);
}

static void *alloc_for_gpu(void *p, uint32_t align, uint32_t size) {
    (void)p;
    uint32_t blk_size = ALIGN_MEM(size, PHYCONT_MEM_ALIGNMENT);
    SceUID blk = sceKernelAllocMemBlock("av_blk", SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_PHYCONT_NC_RW,
                                        blk_size, NULL);
    if (blk < 0) {
        l_error("[video] frame alloc %u bytes (block %u KB) failed: %#x, phycont free %u KB",
                (unsigned)size, (unsigned)(blk_size / 1024), blk, phycont_free_kb());
        return NULL;
    }
    void *base = NULL;
    sceKernelGetMemBlockBase(blk, &base);
    for (int i = 0; i < GPU_ALLOC_SLOTS; i++) {
        if (!g_gpu_allocs[i].base) {
            g_gpu_allocs[i].blk = blk;
            g_gpu_allocs[i].base = base;
            break;
        }
    }
    int ret = sceGxmMapMemory(base, blk_size, SCE_GXM_MEMORY_ATTRIB_RW);
    if (ret < 0)
        l_error("[video] sceGxmMapMemory(%p, %u) failed: %#x", base, (unsigned)blk_size, ret);
    l_info("[video] frame alloc %u bytes align %u -> %p (block %u KB, phycont free %u KB)",
           (unsigned)size, (unsigned)align, base, (unsigned)(blk_size / 1024), phycont_free_kb());
    return base;
}

static void free_for_gpu(void *p, void *addr) {
    (void)p;
    SceUID blk = sceKernelFindMemBlockByAddr(addr, 0);
    if (blk < 0) {
        l_error("[video] free_for_gpu(%p): not ours (%#x)", addr, blk);
        return;
    }
    glFinish();
    sceGxmUnmapMemory(addr);
    sceKernelFreeMemBlock(blk);
    for (int i = 0; i < GPU_ALLOC_SLOTS; i++) {
        if (g_gpu_allocs[i].base == addr) {
            g_gpu_allocs[i].base = NULL;
            g_gpu_allocs[i].blk = 0;
        }
    }
}

#define VIDEO_AUDIO_FRAME_SAMPLES 1024

static int audio_thread(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    int port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_MAIN, VIDEO_AUDIO_FRAME_SAMPLES,
                                   48000, SCE_AUDIO_OUT_MODE_STEREO);
    if (port < 0) {
        l_error("[video] sceAudioOutOpenPort failed: %#x", port);
        return sceKernelExitDeleteThread(0);
    }
    int vol[2] = { SCE_AUDIO_VOLUME_0DB, SCE_AUDIO_VOLUME_0DB };
    sceAudioOutSetVolume(port, SCE_AUDIO_VOLUME_FLAG_L_CH | SCE_AUDIO_VOLUME_FLAG_R_CH, vol);

    int cur_rate = 48000, cur_mode = SCE_AUDIO_OUT_MODE_STEREO;
    while (g_audio_run) {
        SceAvPlayerFrameInfo frame;
        if (sceAvPlayerIsActive(g_player) && sceAvPlayerGetAudioData(g_player, &frame)) {
            int rate = (int)frame.details.audio.sampleRate;
            int mode = frame.details.audio.channelCount == 1 ? SCE_AUDIO_OUT_MODE_MONO
                                                              : SCE_AUDIO_OUT_MODE_STEREO;
            if (rate != cur_rate || mode != cur_mode) {
                int ret = sceAudioOutSetConfig(port, VIDEO_AUDIO_FRAME_SAMPLES, rate, mode);
                if (ret < 0)
                    l_error("[video] sceAudioOutSetConfig(%d, %d, %d) failed: %#x",
                            VIDEO_AUDIO_FRAME_SAMPLES, rate, mode, ret);
                cur_rate = rate;
                cur_mode = mode;
            }
            sceAudioOutOutput(port, frame.pData);
            g_audio_blocks++;
            g_audio_mode = mode;
        } else {
            sceKernelDelayThread(1000);
        }
    }
    sceAudioOutOutput(port, NULL);
    sceAudioOutReleasePort(port);
    return sceKernelExitDeleteThread(0);
}

static const char VIDEO_VS[] =
    "void main(float2 pos, float2 uv,"
    "          float2 out vUv : TEXCOORD0, float4 out vPos : POSITION)"
    "{ vPos = float4(pos, 0.0f, 1.0f); vUv = uv; }";

static const char VIDEO_FS[] =
    "uniform sampler2D tex;"
    "float4 main(float2 vUv : TEXCOORD0) : COLOR { return tex2D(tex, vUv); }";

static const float VIDEO_QUAD_POS[8] = { -1.f, 1.f,  1.f, 1.f,  -1.f, -1.f,  1.f, -1.f };
static const float VIDEO_QUAD_UV[8]  = {  0.f, 0.f,  1.f, 0.f,   0.f,  1.f,  1.f,  1.f };

static GLuint compile(GLenum type, const char *src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512] = "";
        glGetShaderInfoLog(s, sizeof(log), NULL, log);
        l_error("[video] shader compile failed: %s", log);
        glDeleteShader(s);
        return 0;
    }
    return s;
}

static bool gl_setup(void) {
    if (g_gl_ready)
        return true;

    GLuint vs = compile(GL_CG_VERTEX_SHADER_EXT, VIDEO_VS);
    GLuint fs = compile(GL_CG_FRAGMENT_SHADER_EXT, VIDEO_FS);
    if (!vs || !fs)
        return false;
    g_program = glCreateProgram();
    glAttachShader(g_program, vs);
    glAttachShader(g_program, fs);
    glBindAttribLocation(g_program, 0, "pos");
    glBindAttribLocation(g_program, 1, "uv");
    glLinkProgram(g_program);
    GLint ok = 0;
    glGetProgramiv(g_program, GL_LINK_STATUS, &ok);
    if (!ok) {
        l_error("[video] program link failed");
        return false;
    }
    g_uniform_tex = glGetUniformLocation(g_program, "tex");

    GLint prev_tex = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev_tex);
    glGenTextures(VIDEO_BUFFERS, g_tex);
    for (int i = 0; i < VIDEO_BUFFERS; i++) {
        glBindTexture(GL_TEXTURE_2D, g_tex[i]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 8, 8, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        g_gxm_tex[i] = vglGetGxmTexture(GL_TEXTURE_2D);
        vglFree(vglGetTexDataPointer(GL_TEXTURE_2D));
    }
    glBindTexture(GL_TEXTURE_2D, (GLuint)prev_tex);
    g_gl_ready = true;
    return true;
}

static void draw_frame(void) {
    GLint prev_program, prev_active, prev_tex, prev_array_buf, prev_viewport[4];
    GLint attr0_on, attr1_on;
    glGetIntegerv(GL_CURRENT_PROGRAM, &prev_program);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &prev_active);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &prev_array_buf);
    glGetIntegerv(GL_VIEWPORT, prev_viewport);
    glGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &attr0_on);
    glGetVertexAttribiv(1, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &attr1_on);
    GLboolean blend = glIsEnabled(GL_BLEND), depth = glIsEnabled(GL_DEPTH_TEST),
              scissor = glIsEnabled(GL_SCISSOR_TEST), cull = glIsEnabled(GL_CULL_FACE);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev_tex);

    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_CULL_FACE);
    glViewport(0, 0, SCREEN_NATIVE_W, SCREEN_NATIVE_H);
    glUseProgram(g_program);
    glBindTexture(GL_TEXTURE_2D, g_tex[g_frame_idx]);
    glUniform1i(g_uniform_tex, 0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, VIDEO_QUAD_POS);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 0, VIDEO_QUAD_UV);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    subtitles_draw();

    if (!attr0_on) glDisableVertexAttribArray(0);
    if (!attr1_on) glDisableVertexAttribArray(1);
    glBindBuffer(GL_ARRAY_BUFFER, (GLuint)prev_array_buf);
    glBindTexture(GL_TEXTURE_2D, (GLuint)prev_tex);
    glActiveTexture((GLenum)prev_active);
    glUseProgram((GLuint)prev_program);
    glViewport(prev_viewport[0], prev_viewport[1], prev_viewport[2], prev_viewport[3]);
    if (blend) glEnable(GL_BLEND);
    if (depth) glEnable(GL_DEPTH_TEST);
    if (scissor) glEnable(GL_SCISSOR_TEST);
    if (cull) glEnable(GL_CULL_FACE);
}

static void resolve_exports(void) {
    if (g_Stopped)
        return;
    g_Stopped = (so_video_cb_fn)so_symbol(&so_mod,
        "Java_com_taggames_moflow_nativeinterface_CVideoPlayerNativeInterface_Stopped");
    g_Dismissed = (so_video_cb_fn)so_symbol(&so_mod,
        "Java_com_taggames_moflow_nativeinterface_CVideoPlayerNativeInterface_Dismissed");
    g_UpdateSubtitles = (so_video_cb_fn)so_symbol(&so_mod,
        "Java_com_taggames_moflow_nativeinterface_CVideoPlayerNativeInterface_UpdateSubtitles");
    if (!g_Stopped || !g_Dismissed || !g_UpdateSubtitles)
        l_error("[video] Stopped=%p Dismissed=%p UpdateSubtitles=%p missing from the game",
                g_Stopped, g_Dismissed, g_UpdateSubtitles);
}

static void fire_completion(bool dismissed, bool engine_locked) {
    resolve_exports();
    if (!engine_locked)
        pthread_mutex_lock(&g_engine_call_mutex);
    if (dismissed && g_Dismissed) g_Dismissed(&jni, NULL);
    if (g_Stopped) g_Stopped(&jni, NULL);
    if (!engine_locked)
        pthread_mutex_unlock(&g_engine_call_mutex);
}

static void stop_player(void) {
    if (g_audio_thid >= 0) {
        g_audio_run = 0;
        sceKernelWaitThreadEnd(g_audio_thid, NULL, NULL);
        g_audio_thid = -1;
    }
    sceAvPlayerStop(g_player);
    sceAvPlayerClose(g_player);
    for (int i = 0; i < GPU_ALLOC_SLOTS; i++) {
        if (g_gpu_allocs[i].base) {
            l_warn("[video] frame %p not freed by the decoder; freeing", g_gpu_allocs[i].base);
            free_for_gpu(NULL, g_gpu_allocs[i].base);
        }
    }
    log_free_memory("after video close");
}

static void finish(bool dismissed, bool engine_locked) {
    uint64_t played_ms = (sceKernelGetProcessTimeWide() - g_start_us) / 1000;
    l_info("[video] %s after %llu ms (duration %llu ms, %u audio blocks)",
           dismissed ? "dismissed" : "finished",
           (unsigned long long)played_ms, (unsigned long long)g_duration_ms,
           g_audio_blocks);
    stop_player();
    subtitles_clear();
    g_state = VIDEO_IDLE;
    fire_completion(dismissed, engine_locked);
}

static bool open_clip(const char *path) {
    static bool avplayer_loaded;
    int ret;
    if (!avplayer_loaded) {
        ret = sceSysmoduleLoadModule(SCE_SYSMODULE_AVPLAYER);
        if (ret < 0) {
            l_error("[video] sceSysmoduleLoadModule(AVPLAYER) failed: %#x", ret);
            return false;
        }
        avplayer_loaded = true;
    }

    SceIoStat st;
    if (!path || sceIoGetstat(path, &st) < 0) {
        l_error("[video] file not found: \"%s\"", path ? path : "(null)");
        return false;
    }

    if (!gl_setup()) {
        l_error("[video] GL setup failed");
        return false;
    }

    SceAvPlayerInitData init;
    memset(&init, 0, sizeof(init));
    init.memoryReplacement.allocate = alloc_for_cpu;
    init.memoryReplacement.deallocate = free_for_cpu;
    init.memoryReplacement.allocateTexture = alloc_for_gpu;
    init.memoryReplacement.deallocateTexture = free_for_gpu;
    init.eventReplacement.eventCallback = player_event;
    init.basePriority = 0xA0;
    init.numOutputVideoFrameBuffers = VIDEO_BUFFERS;
    init.autoStart = SCE_TRUE;
    g_player = sceAvPlayerInit(&init);
    if (!g_player) {
        l_error("[video] sceAvPlayerInit failed: %#x", g_player);
        return false;
    }

    ret = sceAvPlayerAddSource(g_player, path);
    if (ret < 0) {
        l_error("[video] sceAvPlayerAddSource failed: %#x", ret);
        sceAvPlayerClose(g_player);
        return false;
    }

    g_saw_release = false;
    g_frame_idx = 0;
    g_duration_ms = 0;
    g_frame_w = g_frame_h = 0;
    g_first_frame_us = 0;
    g_audio_blocks = 0;
    g_start_us = sceKernelGetProcessTimeWide();

    g_audio_run = 1;
    g_audio_thid = sceKernelCreateThread("video_audio", audio_thread,
                                         VIDEO_AUDIO_THREAD_PRIORITY, 16 * 1024, 0, 0, NULL);
    if (g_audio_thid >= 0) {
        sceKernelStartThread(g_audio_thid, 0, NULL);
    } else {
        l_error("[video] audio thread create failed: %#x; playing silent", g_audio_thid);
    }

    g_state = VIDEO_STARTING;
    log_free_memory("after video open");
    return true;
}

#define soak_begin(path) false

void video_present(const char *game_path, bool in_apk, bool can_dismiss, bool has_subtitles) {
    IO_SHORT(path, game_path);
    l_info("[video] present path=\"%s\" apk=%d dismiss=%d subs=%d",
           path ? path : "(null)", in_apk, can_dismiss, has_subtitles);

    if (g_state != VIDEO_IDLE) {
        l_warn("[video] present while playing; completing the previous video");
        finish(true, true);
    }

    g_has_subtitles = has_subtitles;
    if (soak_begin(path))
        return;

    g_can_dismiss = can_dismiss;
    if (!open_clip(path)) {
        l_warn("[video] skipping \"%s\"", path ? path : "(null)");
        fire_completion(true, true);
    }
}

void video_dismiss(void) {
    if (g_state == VIDEO_IDLE)
        return;
    l_info("[video] dismissed by the game");
    finish(true, true);
}

static bool skip_pressed(void) {
    SceTouchData touch;
    bool down = false;
    if (controls_skip_held())
        down = true;
    if (sceTouchPeek(SCE_TOUCH_PORT_FRONT, &touch, 1) > 0 && touch.reportNum > 0)
        down = true;
    if (!down) {
        g_saw_release = true;
        return false;
    }
    return g_saw_release;
}

void video_frame(void) {
    if (g_state == VIDEO_IDLE)
        return;

    if (sceAvPlayerIsActive(g_player)) {
        SceAvPlayerFrameInfo frame;
        if (sceAvPlayerGetVideoData(g_player, &frame)) {
            g_frame_idx = (g_frame_idx + 1) % VIDEO_BUFFERS;
            SceGxmTexture *tex = g_gxm_tex[g_frame_idx];
            sceGxmTextureInitLinear(tex, frame.pData, SCE_GXM_TEXTURE_FORMAT_YVU420P2_CSC1,
                                    frame.details.video.width, frame.details.video.height, 0);
            sceGxmTextureSetMinFilter(tex, SCE_GXM_TEXTURE_FILTER_LINEAR);
            sceGxmTextureSetMagFilter(tex, SCE_GXM_TEXTURE_FILTER_LINEAR);
            if (g_state == VIDEO_STARTING) {
                g_state = VIDEO_PLAYING;
                g_first_frame_us = sceKernelGetProcessTimeWide();
                g_frame_w = frame.details.video.width;
                g_frame_h = frame.details.video.height;
                SceAvPlayerStreamInfo info;
                memset(&info, 0, sizeof(info));
                if (sceAvPlayerGetStreamInfo(g_player, 0, &info) >= 0)
                    g_duration_ms = info.duration;
                l_info("[video] first frame %ux%u after %llu ms, duration %llu ms",
                       (unsigned)g_frame_w, (unsigned)g_frame_h,
                       (unsigned long long)((sceKernelGetProcessTimeWide() - g_start_us) / 1000),
                       (unsigned long long)g_duration_ms);
            }
        }
    } else if (g_state == VIDEO_PLAYING) {
        finish(false, false);
        return;
    } else if (g_state == VIDEO_STARTING &&
               sceKernelGetProcessTimeWide() - g_start_us > VIDEO_FIRST_FRAME_TIMEOUT_US) {
        l_error("[video] no first frame within the timeout; skipping");
        finish(true, false);
        return;
    }

    if (g_state == VIDEO_PLAYING && g_has_subtitles) {
        resolve_exports();
        if (g_UpdateSubtitles) {
            pthread_mutex_lock(&g_engine_call_mutex);
            g_UpdateSubtitles(&jni, NULL);
            pthread_mutex_unlock(&g_engine_call_mutex);
        }
    }

    if (g_state == VIDEO_PLAYING)
        draw_frame();

    if (skip_pressed()) {
        if (g_can_dismiss) {
            finish(true, false);
        } else {
            static bool warned;
            if (!warned) {
                l_info("[video] skip pressed but the game marked this video non-dismissable");
                warned = true;
            }
        }
    }
}

bool video_is_playing(void) {
    return g_state != VIDEO_IDLE;
}

float video_time_s(void) {
    if (g_state == VIDEO_IDLE)
        return 0.f;
    return (float)sceAvPlayerCurrentTime(g_player) / 1000.f;
}

float video_duration_s(void) {
    return (float)g_duration_ms / 1000.f;
}
