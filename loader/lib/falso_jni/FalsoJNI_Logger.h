/*
 * FalsoJNI_Logger.h
 *
 * Fake Java Native Interface, providing JavaVM and JNIEnv objects.
 *
 * Copyright (C) 2022-2023 Volodymyr Atamanenko
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#pragma clang diagnostic push
#pragma ide diagnostic ignored "bugprone-reserved-identifier"

#ifndef FALSOJNI_LOGGER_H
#define FALSOJNI_LOGGER_H

#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef FALSOJNI_DEBUG_NO
#define FALSOJNI_DEBUG_NO    4
#define FALSOJNI_DEBUG_ERROR 3
#define FALSOJNI_DEBUG_WARN  2
#define FALSOJNI_DEBUG_INFO  1
#define FALSOJNI_DEBUG_ALL   0
#endif

#ifndef FALSOJNI_DEBUGLEVEL
#define FALSOJNI_DEBUGLEVEL FALSOJNI_DEBUG_WARN
#endif

#if FALSOJNI_DEBUGLEVEL <= FALSOJNI_DEBUG_INFO
#define fjni_logv_info(fmt, ...)  _fjni_log_info(__FILE__, __LINE__, __func__, fmt, __VA_ARGS__)
#define fjni_log_info(fmt)        _fjni_log_info(__FILE__, __LINE__, __func__, fmt)
#else
#define fjni_logv_info(fmt, ...)  ((void)0)
#define fjni_log_info(fmt)        ((void)0)
#endif

#if FALSOJNI_DEBUGLEVEL <= FALSOJNI_DEBUG_WARN
#define fjni_logv_warn(fmt, ...)  _fjni_log_warn(__FILE__, __LINE__, __func__, fmt, __VA_ARGS__)
#define fjni_log_warn(fmt)        _fjni_log_warn(__FILE__, __LINE__, __func__, fmt)
#else
#define fjni_logv_warn(fmt, ...)  ((void)0)
#define fjni_log_warn(fmt)        ((void)0)
#endif

#if FALSOJNI_DEBUGLEVEL <= FALSOJNI_DEBUG_ALL
#define fjni_logv_dbg(fmt, ...)   _fjni_log_debug(__FILE__, __LINE__, __func__, fmt, __VA_ARGS__)
#define fjni_log_dbg(fmt)         _fjni_log_debug(__FILE__, __LINE__, __func__, fmt)
#else
#define fjni_logv_dbg(fmt, ...)   ((void)0)
#define fjni_log_dbg(fmt)         ((void)0)
#endif

#if FALSOJNI_DEBUGLEVEL <= FALSOJNI_DEBUG_ERROR
#define fjni_logv_err(fmt, ...)   _fjni_log_error(__FILE__, __LINE__, __func__, fmt, __VA_ARGS__)
#define fjni_log_err(fmt)         _fjni_log_error(__FILE__, __LINE__, __func__, fmt)
#else
#define fjni_logv_err(fmt, ...)   ((void)0)
#define fjni_log_err(fmt)         ((void)0)
#endif

void _fjni_log_info(const char *fi, int li, const char *fn, const char* fmt, ...);
void _fjni_log_warn(const char *fi, int li, const char *fn, const char* fmt, ...);
void _fjni_log_debug(const char *fi, int li, const char *fn, const char* fmt, ...);
void _fjni_log_error(const char *fi, int li, const char *fn, const char* fmt, ...);

#ifdef __cplusplus
};
#endif

#endif
