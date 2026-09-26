/*
 * Copyright (C) 2021      Andy Nguyen
 * Copyright (C) 2022      Rinnegatamante
 * Copyright (C) 2022-2023 Volodymyr Atamanenko
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include "reimpl/sys.h"

#include <sys/errno.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/clib.h>
#include <stddef.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/rtc.h>
#include <stdlib.h>

#include "utils/utils.h"
#include "utils/logger.h"

#define BIONIC_CLOCK_REALTIME           0
#define BIONIC_CLOCK_MONOTONIC          1
#define BIONIC_CLOCK_PROCESS_CPUTIME_ID 2
#define BIONIC_CLOCK_THREAD_CPUTIME_ID  3
#define BIONIC_CLOCK_MONOTONIC_RAW      4
#define BIONIC_CLOCK_REALTIME_COARSE    5
#define BIONIC_CLOCK_MONOTONIC_COARSE   6
#define BIONIC_CLOCK_BOOTTIME           7
#define BIONIC_CLOCK_REALTIME_ALARM     8
#define BIONIC_CLOCK_BOOTTIME_ALARM     9
#define BIONIC_CLOCK_SGI_CYCLE         10
#define BIONIC_CLOCK_TAI               11

#define __epoch 62135587294000000

int clock_gettime_soloader(clockid_t clock_id, struct timespec * tp) {
    switch (clock_id) {
        case BIONIC_CLOCK_MONOTONIC:
        case BIONIC_CLOCK_MONOTONIC_RAW:
        case BIONIC_CLOCK_MONOTONIC_COARSE:
        case BIONIC_CLOCK_BOOTTIME:
        case BIONIC_CLOCK_BOOTTIME_ALARM:
        case BIONIC_CLOCK_SGI_CYCLE:
        case BIONIC_CLOCK_PROCESS_CPUTIME_ID:
        case BIONIC_CLOCK_THREAD_CPUTIME_ID: {
            uint64_t proctime = sceKernelGetProcessTimeWide();

            tp->tv_sec = (proctime / 1000000);
            tp->tv_nsec = ((proctime - ((uint64_t) tp->tv_sec * 1000000)) * 1000);
            break;
        }
        case BIONIC_CLOCK_REALTIME:
        case BIONIC_CLOCK_REALTIME_COARSE:
        case BIONIC_CLOCK_REALTIME_ALARM:
        case BIONIC_CLOCK_TAI: {
            SceRtcTick tick;
            sceRtcGetCurrentTick(&tick);
            tick.tick -= __epoch;

            tp->tv_sec = (tick.tick / 1000000);
            tp->tv_nsec = ((tick.tick - ((uint64_t) tp->tv_sec * 1000000)) * 1000);
            break;
        }
        default:
            l_error("clock_gettime / unexpected clock id %li", (long)clock_id);
            errno = EINVAL;
            return -1;
    }

    return 0;
}

int clock_getres_soloader(clockid_t clock_id, struct timespec * res) {
    res->tv_sec = 0;
    res->tv_nsec = 1000;
    return 0;
}

clock_t clock_soloader(void) {
    return sceKernelGetProcessTimeLow();
}

_Static_assert(sizeof(struct tm) == 36, "newlib struct tm is not 36 bytes");
_Static_assert(sizeof(tm_bionic) == 44, "bionic struct tm is not 44 bytes");
_Static_assert(offsetof(tm_bionic, tm_gmtoff) == 36, "tm_gmtoff moved");
_Static_assert(offsetof(tm_bionic, tm_zone) == 40, "tm_zone moved");

static void tm_newlib_to_bionic(const struct tm * src, tm_bionic * dst,
                                int local) {
    dst->tm_sec   = src->tm_sec;
    dst->tm_min   = src->tm_min;
    dst->tm_hour  = src->tm_hour;
    dst->tm_mday  = src->tm_mday;
    dst->tm_mon   = src->tm_mon;
    dst->tm_year  = src->tm_year;
    dst->tm_wday  = src->tm_wday;
    dst->tm_yday  = src->tm_yday;
    dst->tm_isdst = src->tm_isdst;

    if (local) {
        dst->tm_gmtoff = -_timezone;
        dst->tm_zone   = _tzname[src->tm_isdst > 0 ? 1 : 0];
    } else {
        dst->tm_gmtoff = 0;
        dst->tm_zone   = "UTC";
    }
}

static void tm_bionic_to_newlib(const tm_bionic * src, struct tm * dst) {
    dst->tm_sec   = src->tm_sec;
    dst->tm_min   = src->tm_min;
    dst->tm_hour  = src->tm_hour;
    dst->tm_mday  = src->tm_mday;
    dst->tm_mon   = src->tm_mon;
    dst->tm_year  = src->tm_year;
    dst->tm_wday  = src->tm_wday;
    dst->tm_yday  = src->tm_yday;
    dst->tm_isdst = src->tm_isdst;
}

static tm_bionic tm_static;

tm_bionic * gmtime_soloader(const time_t * timep) {
    struct tm * res = gmtime(timep);
    if (!res) return NULL;

    tm_newlib_to_bionic(res, &tm_static, 0);
    return &tm_static;
}

tm_bionic * localtime_soloader(const time_t * timep) {
    struct tm * res = localtime(timep);
    if (!res) return NULL;

    tm_newlib_to_bionic(res, &tm_static, 1);
    return &tm_static;
}

tm_bionic * gmtime_r_soloader(const time_t * timep, tm_bionic * result) {
    if (!result) return NULL;

    struct tm tmp;
    if (!gmtime_r(timep, &tmp)) return NULL;

    tm_newlib_to_bionic(&tmp, result, 0);
    return result;
}

size_t strftime_soloader(char * s, size_t max, const char * format,
                         const tm_bionic * timeptr) {
    if (!timeptr) return 0;

    struct tm tmp;
    tm_bionic_to_newlib(timeptr, &tmp);
    return strftime(s, max, format, &tmp);
}

int sigaction_soloader(int signum, const struct sigaction * act, struct sigaction * oldact) {
    l_warn("sigaction(%i, ...): not implemented", signum);
    return 0;
}

int __system_property_get_soloader(const char *name, char *value) {
    l_warn("__system_property_get(%s, %p): not implemented", name, value);
    strncpy(value, "psvita", 7);
    return 7;
}

void assert2(const char* f, int l, const char* func, const char* msg) {
    l_fatal("[%s:%i][%s] Assertion failed: %s", f, l, func, msg);
}

long syscall_soloader(long number, ...) {
    l_warn("syscall(%ld): not implemented", number);
    errno = ENOSYS;
    return -1;
}

void __stack_chk_fail_soloader() {
    l_fatal("Stack collapsed at address %p", __builtin_return_address(0));
    abort();
}

extern char * __cxa_demangle(const char * mangled_name, char * output_buffer, size_t * length, int * status);

void abort_soloader() {
    void * tinfo = game_cxa_current_exception_type ? game_cxa_current_exception_type() : NULL;
    if (tinfo) {
        const char * mangled = ((const char **) tinfo)[1];
        int status = 0;
        char * demangled = mangled ? __cxa_demangle(mangled, NULL, NULL, &status) : NULL;
        const char * type_name = (status == 0 && demangled) ? demangled : (mangled ? mangled : "(no type info)");
        l_fatal("Abort called from address %p while handling C++ exception: %s",
                __builtin_return_address(0), type_name);

        if (demangled && strncmp(demangled, "CEGUI::", 7) == 0 &&
            game_cxa_get_globals && game_cegui_exception_what) {
            void * header = *(void **) game_cxa_get_globals();
            if (header) {
                if (*((unsigned char *) header + 0x27) == 1) {
                    header = (void *) (*(int *) header - 0x78);
                }
                void * exc_obj = (char *) header + 0x78;
                const char * message = game_cegui_exception_what(exc_obj);
                l_fatal("Exception message: %s", message ? message : "(null)");
            }
        }
        free(demangled);
    } else {
        l_fatal("Abort called from address %p", __builtin_return_address(0));
    }
    abort();
}

extern void * __cxa_throw;

void __cxa_throw_soloader(void * thrown_exception, void * tinfo, void (* dest)(void *)) {
    const char * mangled = tinfo ? ((const char **) tinfo)[1] : NULL;
    if (mangled) {
        int status = 0;
        char * demangled = __cxa_demangle(mangled, NULL, NULL, &status);
        l_fatal("C++ exception thrown (uncaught if this precedes an Abort log): %s",
                (status == 0 && demangled) ? demangled : mangled);
        free(demangled);
    } else {
        l_fatal("C++ exception thrown (uncaught if this precedes an Abort log): (no type info)");
    }

    ((void (*)(void *, void *, void (*)(void *))) &__cxa_throw)(thrown_exception, tinfo, dest);
    __builtin_unreachable();
}

extern void * __cxa_rethrow;
extern void * __cxa_current_exception_type(void);

void __cxa_rethrow_soloader(void) {
    void * tinfo = __cxa_current_exception_type();
    const char * mangled = tinfo ? ((const char **) tinfo)[1] : NULL;
    if (mangled) {
        int status = 0;
        char * demangled = __cxa_demangle(mangled, NULL, NULL, &status);
        l_fatal("C++ exception rethrown (uncaught if this precedes an Abort log): %s",
                (status == 0 && demangled) ? demangled : mangled);
        free(demangled);
    } else {
        l_fatal("C++ exception rethrown (uncaught if this precedes an Abort log): (no type info)");
    }

    ((void (*)(void)) &__cxa_rethrow)();
    __builtin_unreachable();
}

void exit_soloader(int status) {
    l_fatal("Exit(%i) called from %p", status, __builtin_return_address(0));
    exit(status);
}

int __atomic_dec(volatile int *ptr) {
    return __sync_fetch_and_sub(ptr, 1);
}

int __atomic_inc(volatile int *ptr) {
    return __sync_fetch_and_add(ptr, 1);
}

int __atomic_swap(int new_value, volatile int *ptr) {
    int old_value;
    do {
        old_value = *ptr;
    } while (__sync_val_compare_and_swap(ptr, old_value, new_value) != old_value);
    return old_value;
}

int __atomic_cmpxchg(int old_value, int new_value, volatile int* ptr) {
    return __sync_val_compare_and_swap(ptr, old_value, new_value) != old_value;
}

char * getenv_soloader(const char * var) {
    l_warn("getenv(\"%s\"): not implemented.", var);
    return NULL;
}

int setenv_soloader(const char * name, const char * value, int overwrite) {
    l_warn("setenv(\"%s\", \"%s\"): not implemented.", name, value);
    return 0;
}

int getpagesize_soloader(void) {
    return PAGE_SIZE;
}
