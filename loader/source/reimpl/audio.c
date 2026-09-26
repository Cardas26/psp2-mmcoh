/*
 * Copyright (C) 2025 Volodymyr Atamanenko
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include "reimpl/audio.h"
#include "utils/boundary_census.h"

#include "falso_jni/FalsoJNI.h"
#include "utils/init.h"
#include "utils/logger.h"

#include <stdint.h>
#include <string.h>

#include <psp2/audioout.h>
#include <psp2/kernel/threadmgr.h>

#define FMOD_INFO_SAMPLERATE 0
#define FMOD_INFO_BLOCKSIZE  1
#define FMOD_INFO_NUMBUFFERS 2

typedef int (*fmod_get_info_t)(void *env, void *thiz, int info);
typedef int (*fmod_process_t)(void *env, void *thiz, void *buf);
typedef int (*fmod_get_software_format_t)(void *self, int *samplerate, int *format,
                                          int *numoutputchannels, int *maxinputchannels,
                                          int *resamplemethod, int *bits);

#define AUDIO_PUMP_STOP_TIMEOUT_US (1000 * 1000)

#define AUDIO_PUMP_MAX_SAMPLES 2048
#define AUDIO_PUMP_MAX_BYTES   (AUDIO_PUMP_MAX_SAMPLES * 2 * (int)sizeof(int16_t))

#ifndef AUDIO_PUMP_AHEAD
#define AUDIO_PUMP_AHEAD 2
#endif
#define AUDIO_PUMP_RING (AUDIO_PUMP_AHEAD + 1)

static int16_t g_pcm[AUDIO_PUMP_RING][AUDIO_PUMP_MAX_BYTES / sizeof(int16_t)];

static int g_buf_token;

static fmod_process_t g_fmod_process = NULL;
static int g_port = -1;
static int g_pcm_bytes = 0;
static SceUID g_mix_thread = -1;
static SceUID g_out_thread = -1;
static SceUID g_free = -1;
static SceUID g_full = -1;
static volatile int g_pump_run = 0;
static SceUInt g_block_wait_us = 50000;

static int audio_mix_thread(SceSize args, void *argp) {
    int cur = 0;
    while (g_pump_run) {
        SceUInt timeout = g_block_wait_us;
        if (sceKernelWaitSema(g_free, 1, &timeout) < 0)
            continue;
        FalsoJNI_RegisterDirectBuffer((jobject)&g_buf_token, g_pcm[cur], g_pcm_bytes);
        memset(g_pcm[cur], 0, g_pcm_bytes);
        g_fmod_process(&jni, NULL, (void *)&g_buf_token);
        sceKernelSignalSema(g_full, 1);
        cur = (cur + 1) % AUDIO_PUMP_RING;
    }
    FalsoJNI_RegisterDirectBuffer(NULL, NULL, 0);
    return 0;
}

static int audio_out_thread(SceSize args, void *argp) {
    int cur = 0;
    int have_prev = 0;
    while (g_pump_run) {
        int got = 0;
        if (!got) {
            SceUInt timeout = g_block_wait_us;
            if (sceKernelWaitSema(g_full, 1, &timeout) < 0)
                continue;
        }
        sceAudioOutOutput(g_port, g_pcm[cur]);
        if (have_prev)
            sceKernelSignalSema(g_free, 1);
        have_prev = 1;
        cur = (cur + 1) % AUDIO_PUMP_RING;
    }
    return 0;
}

static void audio_pump_teardown(void) {
    if (g_mix_thread >= 0) { sceKernelDeleteThread(g_mix_thread); g_mix_thread = -1; }
    if (g_out_thread >= 0) { sceKernelDeleteThread(g_out_thread); g_out_thread = -1; }
    if (g_free >= 0) { sceKernelDeleteSema(g_free); g_free = -1; }
    if (g_full >= 0) { sceKernelDeleteSema(g_full); g_full = -1; }
    if (g_port >= 0) { sceAudioOutReleasePort(g_port); g_port = -1; }
}

void audio_pump_start(void *fmod_system) {
    if (g_pump_run)
        return;

    uintptr_t get_info = so_symbol(&so_mod_fmodex, "Java_org_fmod_FMODAudioDevice_fmodGetInfo");
    uintptr_t process = so_symbol(&so_mod_fmodex, "Java_org_fmod_FMODAudioDevice_fmodProcess");
    uintptr_t get_format = so_symbol(&so_mod_fmodex,
        "_ZN4FMOD6System17getSoftwareFormatEPiP17FMOD_SOUND_FORMATS1_S1_P18FMOD_DSP_RESAMPLERS1_");
    if (!get_info || !process || !get_format) {
        l_error("[fmod-pump] libfmodex symbols missing (getinfo=%#x process=%#x format=%#x)",
                get_info, process, get_format);
        return;
    }

    fmod_get_info_t gi = (fmod_get_info_t)get_info;
    int rate = gi(NULL, NULL, FMOD_INFO_SAMPLERATE);
    int blocksize = gi(NULL, NULL, FMOD_INFO_BLOCKSIZE);
    int numbuffers = gi(NULL, NULL, FMOD_INFO_NUMBUFFERS);
    if (rate <= 0 || blocksize <= 0) {
        l_error("[fmod-pump] AudioTrack driver not initialised (rate=%d blocksize=%d)",
                rate, blocksize);
        return;
    }

    int fmt_rate = 0, format = 0, channels = 0, maxinputs = 0, resampler = 0, bits = 0;
    int fret = ((fmod_get_software_format_t)get_format)(fmod_system, &fmt_rate, &format,
                                                        &channels, &maxinputs, &resampler, &bits);
    if (fret != 0 || channels < 1) {
        l_error("[fmod-pump] getSoftwareFormat failed (ret=%d channels=%d), assuming stereo",
                fret, channels);
        channels = 2;
    }
    if (channels > 2)
        channels = 2;

    l_warn("[fmod-pump] format: rate=%d blocksize=%d numbuffers=%d channels=%d "
           "(getSoftwareFormat rate=%d format=%d bits=%d)",
           rate, blocksize, numbuffers, channels, fmt_rate, format, bits);

    if (blocksize > AUDIO_PUMP_MAX_SAMPLES || (blocksize % SCE_AUDIO_MIN_LEN) != 0) {
        l_error("[fmod-pump] DSP blocksize %d is not a usable sceAudioOut grain", blocksize);
        return;
    }

    g_pcm_bytes = blocksize * channels * (int)sizeof(int16_t);
    g_port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_BGM, blocksize, rate,
                                 channels == 1 ? SCE_AUDIO_OUT_MODE_MONO
                                               : SCE_AUDIO_OUT_MODE_STEREO);
    if (g_port < 0) {
        l_error("[fmod-pump] sceAudioOutOpenPort(len=%d freq=%d ch=%d) failed: %#x",
                blocksize, rate, channels, g_port);
        g_port = -1;
        return;
    }
    int vol[2] = { SCE_AUDIO_VOLUME_0DB, SCE_AUDIO_VOLUME_0DB };
    sceAudioOutSetVolume(g_port, SCE_AUDIO_VOLUME_FLAG_L_CH | SCE_AUDIO_VOLUME_FLAG_R_CH, vol);

    g_fmod_process = (fmod_process_t)process;
    g_block_wait_us = (SceUInt)((uint64_t)blocksize * 1000000u / (uint32_t)rate) * 2;
    g_free = sceKernelCreateSema("fmod_pump_free", 0, AUDIO_PUMP_RING, AUDIO_PUMP_RING, NULL);
    g_full = sceKernelCreateSema("fmod_pump_full", 0, 0, AUDIO_PUMP_RING, NULL);
    if (g_free < 0 || g_full < 0) {
        l_error("[fmod-pump] sceKernelCreateSema failed: free=%#x full=%#x", g_free, g_full);
        if (g_free < 0) g_free = -1;
        if (g_full < 0) g_full = -1;
        audio_pump_teardown();
        return;
    }
    g_pump_run = 1;
    g_mix_thread = sceKernelCreateThread("fmod_pump_mix", audio_mix_thread,
                                         AUDIO_PUMP_PRIORITY + 1, 256 * 1024, 0, 0, NULL);
    g_out_thread = sceKernelCreateThread("fmod_pump_out", audio_out_thread,
                                         AUDIO_PUMP_PRIORITY, 16 * 1024, 0, 0, NULL);
    if (g_mix_thread < 0 || g_out_thread < 0) {
        l_error("[fmod-pump] sceKernelCreateThread failed: mix=%#x out=%#x", g_mix_thread, g_out_thread);
        if (g_mix_thread < 0) g_mix_thread = -1;
        if (g_out_thread < 0) g_out_thread = -1;
        g_pump_run = 0;
        audio_pump_teardown();
        return;
    }
    int s1 = sceKernelStartThread(g_out_thread, 0, NULL);
    int s2 = sceKernelStartThread(g_mix_thread, 0, NULL);
    if (s1 < 0 || s2 < 0) {
        l_error("[fmod-pump] sceKernelStartThread failed: out=%#x mix=%#x", s1, s2);
        g_pump_run = 0;
        if (s1 >= 0 || s2 >= 0) {
            SceUInt timeout = AUDIO_PUMP_STOP_TIMEOUT_US;
            if (s1 >= 0) sceKernelWaitThreadEnd(g_out_thread, NULL, &timeout);
            timeout = AUDIO_PUMP_STOP_TIMEOUT_US;
            if (s2 >= 0) sceKernelWaitThreadEnd(g_mix_thread, NULL, &timeout);
        }
        audio_pump_teardown();
        return;
    }
    l_warn("[fmod-pump] started: port=%d mix=%#x out=%#x %d bytes/block, %d block(s) ahead",
           g_port, g_mix_thread, g_out_thread, g_pcm_bytes, AUDIO_PUMP_AHEAD);
    boundary_census_set_mix_tid(g_mix_thread);
}

void audio_pump_stop(void) {
    if (!g_pump_run)
        return;
    g_pump_run = 0;
    SceUInt timeout = AUDIO_PUMP_STOP_TIMEOUT_US;
    int w1 = sceKernelWaitThreadEnd(g_mix_thread, NULL, &timeout);
    timeout = AUDIO_PUMP_STOP_TIMEOUT_US;
    int w2 = sceKernelWaitThreadEnd(g_out_thread, NULL, &timeout);

    if (w1 < 0 || w2 < 0) {
        l_error("[fmod-pump] threads did not exit within %u us (mix %#x, out %#x); "
                "leaking them and port %d rather than deleting a live thread",
                (unsigned)AUDIO_PUMP_STOP_TIMEOUT_US, w1, w2, g_port);
        g_mix_thread = -1;
        g_out_thread = -1;
        g_free = -1;
        g_full = -1;
        g_port = -1;
        return;
    }

    audio_pump_teardown();
    l_warn("[fmod-pump] stopped");
}
