#include "utils/loading_drain.h"

#include <so_util/so_util.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <stdint.h>

#include "utils/logger.h"

extern so_module so_mod;

#ifndef LOADING_SCREEN_DRAIN_MS
#define LOADING_SCREEN_DRAIN_MS 8
#endif

static so_hook g_hook;
static void (*g_execute)(void);

static int w_loading_update(void *self, uint32_t dt_bits) {
    int r = SO_CONTINUE(int, g_hook, self, dt_bits);
    uint64_t until = sceKernelGetProcessTimeWide() + (uint64_t)LOADING_SCREEN_DRAIN_MS * 1000;
    while (sceKernelGetProcessTimeWide() < until) {
        g_execute();
        sceKernelDelayThread(250);
    }
    return r;
}

void loading_drain_install(void) {
    uintptr_t update  = so_symbol(&so_mod, "_ZN16GameStateMachine13loadingUpdateEf");
    uintptr_t execute = so_symbol(&so_mod, "_ZN5moFlo14CTaskScheduler22ExecuteMainThreadTasksEv");
    if (!update || !execute) {
        l_warn("[loading-drain] loadingUpdate %p / ExecuteMainThreadTasks %p - not installed",
               (void *)update, (void *)execute);
        return;
    }
    g_execute = (void (*)(void))execute;
    g_hook = hook_addr(update, (uintptr_t)w_loading_update);
    l_perf("[loading-drain] installed, %d ms per loading-screen frame", LOADING_SCREEN_DRAIN_MS);
}
