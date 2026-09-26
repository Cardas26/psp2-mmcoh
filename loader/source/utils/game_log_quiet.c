#if defined(GAME_LOG_QUIET)

#include <stdint.h>
#include <so_util/so_util.h>
#include "utils/logger.h"
#include "utils/lazy_lwmutex.h"
#include "utils/game_log_quiet.h"

extern so_module so_mod;

#define SYM_VERBOSE "_ZN5moFlo8CLogging10LogVerboseERKSs"
#define SYM_WARNING "_ZN5moFlo8CLogging10LogWarningERKSs"

static so_hook g_hook_verbose;
static so_hook g_hook_warning;

static lazy_lwmutex_t g_log_lock = LAZY_LWMUTEX_INITIALIZER;

static inline int quiet(void) { return 1; }

static void w_log_verbose(void *str) {
    if (quiet()) return;
    if (!lazy_lwmutex_lock(&g_log_lock, "log_quiet")) return;
    SO_CONTINUE(int, g_hook_verbose, str);
    lazy_lwmutex_unlock(&g_log_lock);
}
static void w_log_warning(void *str) {
    if (quiet()) return;
    if (!lazy_lwmutex_lock(&g_log_lock, "log_quiet")) return;
    SO_CONTINUE(int, g_hook_warning, str);
    lazy_lwmutex_unlock(&g_log_lock);
}

void game_log_quiet_install(void) {
    uintptr_t v = so_symbol(&so_mod, SYM_VERBOSE);
    uintptr_t w = so_symbol(&so_mod, SYM_WARNING);
    if (v) g_hook_verbose = hook_addr(v, (uintptr_t)&w_log_verbose);
    if (w) g_hook_warning = hook_addr(w, (uintptr_t)&w_log_warning);
    l_info("[game-log-quiet] installed: LogVerbose=%s LogWarning=%s%s",
           v ? "hooked" : "MISSING", w ? "hooked" : "MISSING",
           game_log_quiet_ab_arm() < 0 ? "" : " (A/B, starts quiet)");
}

#endif
