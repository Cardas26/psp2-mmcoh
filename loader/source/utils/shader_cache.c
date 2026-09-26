#include "utils/shader_cache.h"

#include "utils/build_id.h"
#include "utils/logger.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <psp2/io/dirent.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>
#include <vitaGL.h>

#ifndef VITAGL_FLAGS_STR
#define VITAGL_FLAGS_STR "unknown"
#endif

#define SHADER_CACHE_ROOT DATA_PATH "shader_cache"
#define SHADER_CACHE_STAMP SHADER_CACHE_ROOT "/build.id"

#define MAX_PURGE_DEPTH 2

static void stamp_text(char * out, size_t out_sz) {
    snprintf(out, out_sz, "%s\n%s\n", g_build_id, VITAGL_FLAGS_STR);
}

static bool stamp_matches(void) {
    char want[512];
    stamp_text(want, sizeof(want));

    SceUID f = sceIoOpen(SHADER_CACHE_STAMP, SCE_O_RDONLY, 0777);
    if (f < 0)
        return false;

    char have[512];
    int n = sceIoRead(f, have, sizeof(have) - 1);
    sceIoClose(f);
    if (n < 0)
        return false;
    have[n] = '\0';

    return strcmp(want, have) == 0;
}

static int purge_gxp(const char * dir, int depth) {
    SceUID d = sceIoDopen(dir);
    if (d < 0)
        return 0;

    int removed = 0;
    SceIoDirent e;
    memset(&e, 0, sizeof(e));
    while (sceIoDread(d, &e) > 0) {
        if (e.d_name[0] == '.')
            continue;

        char path[512];
        int n = snprintf(path, sizeof(path), "%s/%s", dir, e.d_name);
        if (n < 0 || (size_t)n >= sizeof(path))
            continue;

        if (SCE_S_ISDIR(e.d_stat.st_mode)) {
            if (depth > 0)
                removed += purge_gxp(path, depth - 1);
        } else {
            size_t len = strlen(e.d_name);
            if (len > 4 && strcmp(e.d_name + len - 4, ".gxp") == 0
                && sceIoRemove(path) >= 0)
                removed++;
        }
        memset(&e, 0, sizeof(e));
    }
    sceIoDclose(d);
    return removed;
}

void shader_cache_prepare(void) {
    sceIoMkdir(SHADER_CACHE_ROOT, 0777);

    if (!stamp_matches()) {
        int n = purge_gxp(SHADER_CACHE_ROOT, MAX_PURGE_DEPTH);
        l_perf("[shadercache] stamp mismatch, purged %d cached shader(s)", n);
        sceIoRemove(SHADER_CACHE_STAMP);
    }

    vglSetShaderCachePath(SHADER_CACHE_ROOT);
}

void shader_cache_commit(void) {
    if (stamp_matches())
        return;

    char text[512];
    stamp_text(text, sizeof(text));

    SceUID f = sceIoOpen(SHADER_CACHE_STAMP,
                         SCE_O_CREAT | SCE_O_WRONLY | SCE_O_TRUNC, 0777);
    if (f < 0) {
        l_perf("[shadercache] could not write %s (0x%08X)",
               SHADER_CACHE_STAMP, (unsigned)f);
        return;
    }
    sceIoWrite(f, text, strlen(text));
    sceIoClose(f);
}

void shader_cache_time_begin(void) {}
void shader_cache_time_end(const char * what) { (void)what; }
void shader_cache_report(void) {}
