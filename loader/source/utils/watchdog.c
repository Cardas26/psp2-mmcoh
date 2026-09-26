#include "utils/watchdog.h"

#include "utils/logger.h"

#include <psp2/io/fcntl.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <stdatomic.h>
#include <stdint.h>

#ifdef HANG_WATCHDOG

#define WD_TICK_MS       1000
#define WD_HEARTBEAT_MS  5000
#define WD_HANG_MS       300000

#define WD_LOG_PATH      DATA_PATH "watchdog.log"

#define WD_PRIORITY      0x48

static _Atomic uint32_t wd_activity = 0;
static _Atomic int      wd_running  = 0;
static SceUID           wd_thread_id = 0;
static SceUID           wd_fd = -1;

static _Atomic uint32_t wd_wait_tid      = 0;
static _Atomic uint32_t wd_wait_obj      = 0;
static _Atomic uint32_t wd_wait_owner    = 0;
static _Atomic uint64_t wd_wait_since_us = 0;

void watchdog_note_log_activity(void) {
    if (!atomic_load_explicit(&wd_running, memory_order_acquire))
        return;
    if (sceKernelGetThreadId() == wd_thread_id)
        return;
    atomic_fetch_add_explicit(&wd_activity, 1, memory_order_relaxed);
}

void watchdog_note_wait_begin(uint32_t obj_addr, uint32_t self_tid, uint32_t owner_tid) {
    if (!atomic_load_explicit(&wd_running, memory_order_acquire))
        return;
    atomic_store_explicit(&wd_wait_owner, owner_tid, memory_order_relaxed);
    atomic_store_explicit(&wd_wait_obj, obj_addr, memory_order_relaxed);
    atomic_store_explicit(&wd_wait_since_us, sceKernelGetProcessTimeWide(),
                          memory_order_relaxed);
    atomic_store_explicit(&wd_wait_tid, self_tid, memory_order_release);
}

void watchdog_note_wait_end(void) {
    if (!atomic_load_explicit(&wd_running, memory_order_acquire))
        return;
    atomic_store_explicit(&wd_wait_tid, 0, memory_order_release);
}

static volatile struct {
    char     tag[12];
    uint32_t wait_tid;
    uint32_t wait_obj;
    uint32_t wait_owner;
    uint32_t wait_ms;
    uint32_t idle_ms;
    uint32_t armed;
} wd_fault = { "WDFAULT01", 0, 0, 0, 0, 0, 0 };

__attribute__((noinline, noreturn))
static void wd_trap(uint32_t tid, uint32_t obj, uint32_t owner,
                    uint32_t wait_ms, uint32_t idle_ms) {
    (void)tid; (void)obj; (void)owner; (void)wait_ms; (void)idle_ms;
    __builtin_trap();
}

static void wd_write(const char *fmt, ...) {
    if (wd_fd < 0)
        return;

    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    int n = sceClibVsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n <= 0)
        return;
    if (n > (int)sizeof(buf) - 1)
        n = (int)sizeof(buf) - 1;

    sceIoWrite(wd_fd, buf, n);
}

static int watchdog_thread(SceSize args, void *argp) {
    (void)args; (void)argp;

    wd_fd = sceIoOpen(WD_LOG_PATH,
                      SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
    wd_write("watchdog: thread running (tid 0x%08X), fault after %d s quiet\n",
             sceKernelGetThreadId(), WD_HANG_MS / 1000);

    uint32_t last = atomic_load_explicit(&wd_activity, memory_order_relaxed);
    int idle_ms = 0;
    int since_heartbeat_ms = 0;

    for (;;) {
        sceKernelDelayThread(WD_TICK_MS * 1000);

        uint32_t now = atomic_load_explicit(&wd_activity, memory_order_relaxed);
        if (now != last) {
            last = now;
            idle_ms = 0;
        } else {
            idle_ms += WD_TICK_MS;
        }

        if (idle_ms >= WD_HANG_MS) {
            uint32_t wtid = atomic_load_explicit(&wd_wait_tid, memory_order_acquire);
            uint32_t obj = 0, owner = 0, wait_ms = 0;
            if (wtid != 0) {
                uint64_t since = atomic_load_explicit(&wd_wait_since_us, memory_order_relaxed);
                obj = atomic_load_explicit(&wd_wait_obj, memory_order_relaxed);
                owner = atomic_load_explicit(&wd_wait_owner, memory_order_relaxed);
                wait_ms = (uint32_t)((sceKernelGetProcessTimeWide() - since) / 1000);
            }

            wd_fault.wait_tid   = wtid;
            wd_fault.wait_obj   = obj;
            wd_fault.wait_owner = owner;
            wd_fault.wait_ms    = wait_ms;
            wd_fault.idle_ms    = (uint32_t)idle_ms;
            wd_fault.armed      = 1;

            wd_trap(wtid, obj, owner, wait_ms, (uint32_t)idle_ms);
        }

        since_heartbeat_ms += WD_TICK_MS;
        if (since_heartbeat_ms >= WD_HEARTBEAT_MS) {
            since_heartbeat_ms = 0;
            if (idle_ms == 0)
                wd_write("watchdog: alive - %u log lines\n", now);
        }
    }
}

void watchdog_start(void) {
    if (atomic_load_explicit(&wd_running, memory_order_acquire))
        return;

    SceUID t = sceKernelCreateThread("soloader_watchdog", watchdog_thread,
                                     WD_PRIORITY, 32 * 1024, 0, 0, NULL);
    if (t < 0) {
        l_error("watchdog: could not create thread: 0x%08X", t);
        return;
    }

    wd_thread_id = t;
    atomic_store_explicit(&wd_running, 1, memory_order_release);

    sceKernelStartThread(t, 0, NULL);
    l_info("watchdog: armed - forcing a crash after %d s without log output; "
           "heartbeats in " WD_LOG_PATH, WD_HANG_MS / 1000);
}

#else

#endif
