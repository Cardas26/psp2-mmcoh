#ifndef SOLOADER_SETUP_DATA_H
#define SOLOADER_SETUP_DATA_H

#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct setup_entry {
    char *name;
    uint32_t crc, csize, usize, offset;
    uint16_t method;
} setup_entry;

typedef struct setup_zip {
    const char *path;
    FILE *f;
    setup_entry *entries;
    int count;
} setup_zip;

typedef enum setup_stage {
    SETUP_STAGE_APK,
    SETUP_STAGE_OBB,
    SETUP_STAGE_FIXES,
    SETUP_STAGE_DONE,
} setup_stage;

typedef struct setup_job {
    setup_zip apk, obb;
    const char *out;
    volatile uint32_t bytes_done, bytes_total;
    volatile int files_done, files_total;
    volatile setup_stage stage;
    char error[512];
} setup_job;

int setup_open(setup_job *job, const char *apk, const char *obb, const char *out);

int setup_extract(setup_job *job);

void setup_close(setup_job *job);

#ifdef __cplusplus
};
#endif

#endif
