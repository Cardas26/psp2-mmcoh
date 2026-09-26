/*
 * Copyright (C) 2021      Rinnegatamante
 * Copyright (C) 2022-2024 Volodymyr Atamanenko
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include "utils/utils.h"
#include "utils/logger.h"

#include <psp2/io/stat.h>
#include <psp2/kernel/sysmem.h>

#include <errno.h>
#include <malloc.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>

#include <sha1/sha1.h>

uint64_t current_timestamp_ms() {
    struct timeval te;
    gettimeofday(&te, NULL);
    return (te.tv_sec * 1000LL + te.tv_usec / 1000);
}

bool file_exists(const char * path) {
    SceIoStat stat;
    return sceIoGetstat(path, &stat) >= 0;
}

bool file_load(const char * path, uint8_t ** buffer, size_t * size) {
    if (!buffer || !size) {
        l_error("file_load: Invalid argument(s).");
        return false;
    }

    if (!file_exists(path)) {
        l_error("file_load: Specified source path \"%s\" "
                "does not exist.", path);
        return false;
    }

    FILE * f = fopen(path, "rb");

    if (!f) {
        l_error("file_load: Could not open the specified "
                "source path \"%s\".", path);
        return false;
    }

    long len;
    fseek(f, 0, SEEK_END);
    len = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (len <= 0) {
        l_error("file_load: The specified source file \"%s\" is empty or "
                "its size could not be determined (ftell returned %ld).",
                path, len);
        fclose(f);
        return false;
    }

    *size = (size_t)len;
    *buffer = malloc(*size);

    if (!*buffer) {
        l_error("file_load: Unable to allocate %zu bytes of memory to load "
                "the specified source file \"%s\".", *size, path);
        fclose(f);
        return false;
    }

    size_t got = fread(*buffer, 1, *size, f);
    fclose(f);

    if (got != *size) {
        l_error("file_load: Short read on \"%s\": got %zu of %zu bytes.",
                path, got, *size);
        free(*buffer);
        *buffer = NULL;
        *size = 0;
        return false;
    }

    return true;
}

bool file_mkpath(const char * path, mode_t mode) {
    if (!path || !*path) {
        l_error("file_mkpath: Invalid argument.");
        return false;
    }

    char * file_path = strdup(path);
    if (!file_path) {
        l_error("file_mkpath: Out of memory duplicating \"%s\".", path);
        return false;
    }

    for (char* p = strchr(file_path + 1, '/'); p; p = strchr(p + 1, '/')) {
        *p = '\0';
        if (mkdir(file_path, mode) == -1) {
            if (errno != EEXIST) {
                l_error("file_mkpath: Unable to create a directory \"%s\", "
                        "mkdir error code is %s.", file_path, strerror(errno));
                free(file_path);
                return false;
            }
        }
        *p = '/';
    }

    free(file_path);
    return true;
}

bool file_save(const char * path, const uint8_t * buffer, size_t size) {
    FILE * f = fopen(path, "wb");

    if (!f) {
        l_error("file_save: Could not open the specified "
                "target path \"%s\".", path);
        return false;
    }

    size_t written = fwrite(buffer, 1, size, f);
    int closed = fclose(f);

    if (written != size) {
        l_error("file_save: Short write to \"%s\": wrote %zu of %zu bytes.",
                path, written, size);
        return false;
    }

    if (closed != 0) {
        l_error("file_save: Failed to flush and close \"%s\".", path);
        return false;
    }

    return true;
}

SceUID _vshKernelSearchModuleByName(const char *, int *);

bool module_loaded(const char * name) {
    int search_unk[2];
    return _vshKernelSearchModuleByName(name, search_unk) >= 0;
}

int ret0(void) {
    return 0;
}

int ret1(void) {
    return 1;
}

char * str_sha1sum(const char * str, size_t size) {
    if (size == 0) {
        size = strlen(str);
    }

    uint8_t sha1[20];
    SHA1_CTX ctx;
    sha1_init(&ctx);
    sha1_update(&ctx, (uint8_t *)str, size);
    sha1_final(&ctx, (uint8_t *)sha1);

    char hash[42];
    memset(hash, 0, sizeof(hash));

    for (int i = 0; i < 20; i++) {
        char string[4];
        sprintf(string, "%02X", sha1[i]);
        strcat(hash, string);
    }

    hash[41] = '\0';

    char * ret = strdup(hash);
    if (!ret)
        l_error("str_sha1sum: Out of memory duplicating the hash.");

    return ret;
}

void log_free_memory(const char * tag) {
    SceKernelFreeMemorySizeInfo info;
    info.size = sizeof(info);

    int ret = sceKernelGetFreeMemorySize(&info);
    if (ret < 0) {
        l_debug("mem[%s]: sceKernelGetFreeMemorySize failed: 0x%08X", tag, ret);
        return;
    }

    struct mallinfo mi = mallinfo();

    l_debug("mem[%s]: user=%d KB cdram=%d KB phycont=%d KB | "
            "newlib arena=%d KB used=%d KB free=%d KB",
            tag,
            info.size_user / 1024, info.size_cdram / 1024,
            info.size_phycont / 1024,
            mi.arena / 1024, mi.uordblks / 1024, mi.fordblks / 1024);
}
