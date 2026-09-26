/*
 * Copyright (C) 2021      Andy Nguyen
 * Copyright (C) 2021-2022 Rinnegatamante
 * Copyright (C) 2022-2024 Volodymyr Atamanenko
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include "utils/init.h"

#include "utils/dialog.h"
#include "utils/first_boot.h"
#include "utils/glutil.h"
#include "utils/logger.h"
#include "utils/utils.h"

#include <reimpl/controls.h>
#include <reimpl/pthr.h>

#include <psp2/power.h>

#include <falso_jni/FalsoJNI.h>
#include <so_util/so_util.h>
#include <fios/fios.h>

#define LOAD_ADDRESS 0x98000000

extern so_module so_mod;
so_module so_mod_fmodex;
so_module so_mod_fmodevent;

void soloader_init_all() {
    pthr_init();

    int clk_arm  = scePowerSetArmClockFrequency(444);
    int clk_bus  = scePowerSetBusClockFrequency(222);
    int clk_gpu  = scePowerSetGpuClockFrequency(GPU_CLOCK_MHZ);
    int clk_xbar = scePowerSetGpuXbarClockFrequency(166);
    if (clk_arm < 0 || clk_bus < 0 || clk_gpu < 0 || clk_xbar < 0) {
        l_fatal("overclock: partial failure, running below the assumed "
                "performance floor - arm(444)=0x%08X bus(222)=0x%08X "
                "gpu(%d)=0x%08X gpu_xbar(166)=0x%08X",
                clk_arm, clk_bus, GPU_CLOCK_MHZ, clk_gpu, clk_xbar);
    }

    first_boot_run();

    if (!module_loaded("kubridge")) {
        l_fatal("kubridge is not loaded.");
        fatal_error("Error: kubridge.skprx is not installed.");
    }
    l_success("kubridge check passed.");

    if (!file_exists(SO_PATH)) {
        fatal_error("Looks like you haven't installed the data files for this "
                    "port, or they are in an incorrect location. Please make "
                    "sure that you have %s file exactly at that path.", SO_PATH);
    }

    if (!file_exists(FMODEX_PATH) || !file_exists(FMODEVENT_PATH)) {
        fatal_error("Error: could not find %s and/or %s. These ship inside "
                    "the original APK's lib/armeabi/ but are not part of the "
                    "OBB layout - extract them alongside libApplication.so.",
                    FMODEX_PATH, FMODEVENT_PATH);
    }

    if (so_file_load(&so_mod_fmodex, FMODEX_PATH, FMODEX_LOAD_ADDRESS) < 0) {
        l_fatal("libfmodex.so could not be loaded.");
        fatal_error("Error: could not load %s.", FMODEX_PATH);
    }

    if (so_file_load(&so_mod_fmodevent, FMODEVENT_PATH, FMODEVENT_LOAD_ADDRESS) < 0) {
        l_fatal("libfmodevent.so could not be loaded.");
        fatal_error("Error: could not load %s.", FMODEVENT_PATH);
    }

    if (so_file_load(&so_mod, SO_PATH, LOAD_ADDRESS) < 0) {
        l_fatal("SO could not be loaded.");
        fatal_error("Error: could not load %s.", SO_PATH);
    }

    so_relocate(&so_mod_fmodex);
    so_relocate(&so_mod_fmodevent);
    so_relocate(&so_mod);
    l_success("SO relocated.");

    resolve_imports(&so_mod_fmodex);
    resolve_imports(&so_mod_fmodevent);
    resolve_imports(&so_mod);
    l_success("SO imports resolved.");

    so_patch();
    l_success("SO patched.");

    so_flush_caches(&so_mod_fmodex);
    so_flush_caches(&so_mod_fmodevent);
    so_flush_caches(&so_mod);
    l_success("SO caches flushed.");

    so_initialize(&so_mod_fmodex);
    so_initialize(&so_mod_fmodevent);
    so_initialize(&so_mod);
    l_success("SO initialized.");

    gl_preload();
    l_success("OpenGL preloaded.");

    jni_init();
    l_success("FalsoJNI initialized.");

}
