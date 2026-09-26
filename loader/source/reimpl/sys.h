/*
 * Copyright (C) 2021      Andy Nguyen
 * Copyright (C) 2022      Rinnegatamante
 * Copyright (C) 2022-2023 Volodymyr Atamanenko
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#ifndef SOLOADER_SYS_H
#define SOLOADER_SYS_H

#ifdef __cplusplus
extern "C" {
#endif

#include <signal.h>
#include <sys/time.h>
#include <time.h>

#define PAGE_SIZE 4096

typedef struct {
    int tm_sec;
    int tm_min;
    int tm_hour;
    int tm_mday;
    int tm_mon;
    int tm_year;
    int tm_wday;
    int tm_yday;
    int tm_isdst;
    long int tm_gmtoff;
    const char * tm_zone;
} tm_bionic;

tm_bionic * gmtime_soloader(const time_t * timep);

tm_bionic * localtime_soloader(const time_t * timep);

tm_bionic * gmtime_r_soloader(const time_t * timep, tm_bionic * result);

size_t strftime_soloader(char * s, size_t max, const char * format,
                         const tm_bionic * timeptr);

clock_t clock_soloader(void);

int clock_gettime_soloader(clockid_t clock_id, struct timespec * tp);

int clock_getres_soloader(clockid_t clock_id, struct timespec * res);

int __system_property_get_soloader(const char *name, char *value);

void assert2(const char *f, int l, const char *func, const char *msg);

long syscall_soloader(long number, ...);

__attribute__((noreturn))
void __stack_chk_fail_soloader();

void abort_soloader();

void __cxa_throw_soloader(void * thrown_exception, void * tinfo, void (* dest)(void *));
void __cxa_rethrow_soloader(void);

extern void *(*game_cxa_current_exception_type)(void);

extern void *(*game_cxa_get_globals)(void);
extern const char *(*game_cegui_exception_what)(void *exception_object);

void exit_soloader(int status);

int __atomic_dec(volatile int *ptr);

int __atomic_inc(volatile int *ptr);

int __atomic_swap(int new_value, volatile int *ptr);

int __atomic_cmpxchg(int old_value, int new_value, volatile int* ptr);

char * getenv_soloader(const char * name);

int setenv_soloader(const char * name, const char * value, int overwrite);

int getpagesize_soloader(void);

int sigaction_soloader(int signum, const struct sigaction * act,
                       struct sigaction * oldact);

#ifdef __cplusplus
};
#endif

#endif
