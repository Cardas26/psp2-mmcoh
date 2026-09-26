/*
 * Copyright (C) 2021      Rinnegatamante
 * Copyright (C) 2022-2024 Volodymyr Atamanenko
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#ifndef SOLOADER_UTILS_H
#define SOLOADER_UTILS_H

#ifdef __cplusplus
extern "C" {
#endif

#include <sys/types.h>
#include <stdbool.h>
#include <stdint.h>

#define SCREEN_NATIVE_W 960
#define SCREEN_NATIVE_H 544

uint64_t current_timestamp_ms();

bool file_exists(const char * path);

bool file_load(const char * path, uint8_t ** buffer, size_t * size);

bool file_mkpath(const char * path, mode_t mode);

bool file_save(const char * path, const uint8_t * buffer, size_t size);

bool module_loaded(const char * name);

int ret0(void);

int ret1(void);

char * str_sha1sum(const char * str, size_t size);

void log_free_memory(const char * tag);

#ifdef __cplusplus
};
#endif

#endif
