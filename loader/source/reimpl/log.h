/*
 * Copyright (C) 2009      The Android Open Source Project
 * Copyright (C) 2021      Andy Nguyen
 * Copyright (C) 2022      Rinnegatamante
 * Copyright (C) 2022-2023 Volodymyr Atamanenko
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#ifndef SOLOADER_LOG_H
#define SOLOADER_LOG_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdarg.h>

typedef enum android_LogPriority {
    ANDROID_LOG_UNKNOWN = 0,
    ANDROID_LOG_DEFAULT,
    ANDROID_LOG_VERBOSE,
    ANDROID_LOG_DEBUG,
    ANDROID_LOG_INFO,
    ANDROID_LOG_WARN,
    ANDROID_LOG_ERROR,
    ANDROID_LOG_FATAL,
    ANDROID_LOG_SILENT,
} android_LogPriority;

int __android_log_write(int prio, const char *tag, const char *text);

int __android_log_print(int prio, const char *tag, const char *fmt, ...)
    __attribute__((__format__(printf, 3, 4)));

int __android_log_vprint(int prio, const char *tag, const char *fmt, va_list ap)
    __attribute__((__format__(printf, 3, 0)));

void __android_log_assert(const char* cond, const char* tag, const char* fmt, ...)
__attribute__((__noreturn__)) __attribute__((__format__(printf, 3, 4)));

#ifdef __cplusplus
};
#endif

#endif
