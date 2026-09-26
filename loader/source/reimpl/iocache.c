#include "reimpl/iocache.h"

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <psp2/io/dirent.h>
#include <psp2/io/stat.h>
#include <psp2/types.h>

#include "utils/lazy_lwmutex.h"
#include "utils/logger.h"
#include "utils/update_profile.h"

static const char * const ASSET_ROOTS[] = { "/DLC/", "/assets/" };

#define TABLE_INIT 1024
#define MAX_PATH_LEN 256

typedef enum { ST_UNKNOWN = 0, ST_PRESENT, ST_ABSENT } slot_state_t;

typedef struct {
    uint32_t hash;
    uint32_t size;
    time_t   mtime;
    bool     is_dir;
    uint8_t  state;
    char *   path;
} slot_t;

typedef struct {
    slot_t * slots;
    uint32_t cap;
    uint32_t used;
} table_t;

static table_t g_files = {0};
static table_t g_dirs  = {0};
static lazy_lwmutex_t g_lock = LAZY_LWMUTEX_INITIALIZER;

static uint32_t g_enumerated, g_dirs_absent, g_hit_present, g_hit_absent, g_fell_through;
static uint32_t g_since_report;
#define REPORT_EVERY 128

static uint32_t hash_str(const char * s, size_t n) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) {
        h ^= (uint8_t) s[i];
        h *= 16777619u;
    }
    return h ? h : 1u;
}

static bool table_grow(table_t * t) {
    uint32_t cap = t->cap ? t->cap * 2 : TABLE_INIT;
    slot_t * slots = calloc(cap, sizeof(slot_t));
    if (!slots)
        return false;
    for (uint32_t i = 0; i < t->cap; i++) {
        if (!t->slots[i].hash)
            continue;
        uint32_t j = t->slots[i].hash & (cap - 1);
        while (slots[j].hash)
            j = (j + 1) & (cap - 1);
        slots[j] = t->slots[i];
    }
    free(t->slots);
    t->slots = slots;
    t->cap = cap;
    return true;
}

static slot_t * table_find(table_t * t, const char * path, size_t len, uint32_t h) {
    if (!t->cap)
        return NULL;
    uint32_t i = h & (t->cap - 1);
    while (t->slots[i].hash) {
        if (t->slots[i].hash == h && strncmp(t->slots[i].path, path, len) == 0 &&
            t->slots[i].path[len] == '\0')
            return &t->slots[i];
        i = (i + 1) & (t->cap - 1);
    }
    return NULL;
}

static bool table_put(table_t * t, const char * path, size_t len, uint32_t h,
                      uint32_t size, time_t mtime, bool is_dir, uint8_t state) {
    if ((t->used + 1) * 10 >= t->cap * 7 && !table_grow(t))
        return false;

    uint32_t i = h & (t->cap - 1);
    while (t->slots[i].hash) {
        if (t->slots[i].hash == h && strncmp(t->slots[i].path, path, len) == 0 &&
            t->slots[i].path[len] == '\0')
            break;
        i = (i + 1) & (t->cap - 1);
    }
    if (!t->slots[i].hash) {
        char * copy = malloc(len + 1);
        if (!copy)
            return false;
        memcpy(copy, path, len);
        copy[len] = '\0';
        t->slots[i].path = copy;
        t->slots[i].hash = h;
        t->used++;
    }
    t->slots[i].size = size;
    t->slots[i].mtime = mtime;
    t->slots[i].is_dir = is_dir;
    t->slots[i].state = state;
    return true;
}

static void table_erase(table_t * t, const char * path, size_t len, uint32_t h) {
    slot_t * s = table_find(t, path, len, h);
    if (!s)
        return;
    s->state = ST_UNKNOWN;
    s->size = 0;
    s->is_dir = false;
}

static time_t sce_time(const SceDateTime * d) {
    struct tm tm = {0};
    tm.tm_year = d->year - 1900;
    tm.tm_mon = d->month - 1;
    tm.tm_mday = d->day;
    tm.tm_hour = d->hour;
    tm.tm_min = d->minute;
    tm.tm_sec = d->second;
    tm.tm_isdst = -1;
    time_t t = mktime(&tm);
    return t < 0 ? 0 : t;
}

static bool is_asset_path(const char * path) {
    for (size_t i = 0; i < sizeof(ASSET_ROOTS) / sizeof(ASSET_ROOTS[0]); i++)
        if (strstr(path, ASSET_ROOTS[i]))
            return true;
    return false;
}

static size_t dir_len(const char * path) {
    const char * slash = strrchr(path, '/');
    return slash ? (size_t)(slash - path) : 0;
}

static void enumerate(const char * dir, size_t dlen, uint32_t dhash) {
    char buf[MAX_PATH_LEN];
    if (dlen >= sizeof(buf)) {
        table_put(&g_dirs, dir, dlen, dhash, 0, 0, true, ST_ABSENT);
        return;
    }
    memcpy(buf, dir, dlen);
    buf[dlen] = '\0';

    SceUID fd = sceIoDopen(buf);
    if (fd < 0) {
        table_put(&g_dirs, dir, dlen, dhash, 0, 0, true, ST_ABSENT);
        g_dirs_absent++;
        return;
    }

    SceIoDirent ent;
    for (;;) {
        memset(&ent, 0, sizeof(ent));
        int res = sceIoDread(fd, &ent);
        if (res <= 0)
            break;
        size_t nlen = strlen(ent.d_name);
        if (dlen + 1 + nlen >= sizeof(buf))
            continue;
        buf[dlen] = '/';
        memcpy(buf + dlen + 1, ent.d_name, nlen + 1);
        size_t flen = dlen + 1 + nlen;
        table_put(&g_files, buf, flen, hash_str(buf, flen),
                  (uint32_t) ent.d_stat.st_size, sce_time(&ent.d_stat.st_mtime),
                  SCE_S_ISDIR(ent.d_stat.st_mode), ST_PRESENT);
    }
    sceIoDclose(fd);

    table_put(&g_dirs, dir, dlen, dhash, 0, 0, true, ST_PRESENT);
    g_enumerated++;
}

typedef struct {
    bool due;
    uint32_t enumerated, dirs_absent, entries, present, absent, fell_through;
} report_t;

static report_t take_report(bool force) {
    report_t r = {0};
    return r;
}

static void emit_report(const report_t * r) {
    (void) r;
}

bool iocache_stat(const char * path, struct stat * st, int * res) {
    if (!path || !is_asset_path(path)) {
        g_fell_through++;
        return false;
    }

    size_t plen = strlen(path);
    size_t dlen = dir_len(path);
    if (plen >= MAX_PATH_LEN || dlen == 0) {
        g_fell_through++;
        return false;
    }

    bool locked = lazy_lwmutex_lock(&g_lock, "asset_index");
    if (!locked) {
        g_fell_through++;
        return false;
    }

    uint32_t dhash = hash_str(path, dlen);
    const slot_t * d = table_find(&g_dirs, path, dlen, dhash);
    if (!d || d->state == ST_UNKNOWN) {
        enumerate(path, dlen, dhash);
        d = table_find(&g_dirs, path, dlen, dhash);
    }

    const slot_t * f = (d && d->state == ST_PRESENT)
                     ? table_find(&g_files, path, plen, hash_str(path, plen))
                     : NULL;
    bool found = f && f->state == ST_PRESENT;

    if (found) {
        memset(st, 0, sizeof(*st));
        st->st_mode = (f->is_dir ? S_IFDIR : S_IFREG) | 0555;
        st->st_nlink = 1;
        st->st_size = (off_t) f->size;
        st->st_blksize = 512;
        st->st_blocks = (blkcnt_t)((f->size + 511) / 512);
        st->st_atime = st->st_mtime = st->st_ctime = f->mtime;
        g_hit_present++;
    } else {
        g_hit_absent++;
    }

    report_t r = take_report(false);
    lazy_lwmutex_unlock(&g_lock);
    if (r.due && !uprof_on_update_thread()) {
        emit_report(&r);
    }

    if (!found)
        errno = ENOENT;
    *res = found ? 0 : -1;
    return true;
}

bool iocache_size(const char * path, uint32_t * size_out) {
    if (!path || !is_asset_path(path))
        return false;
    size_t plen = strlen(path);
    if (plen >= MAX_PATH_LEN)
        return false;
    if (!lazy_lwmutex_lock(&g_lock, "asset_index"))
        return false;
    const slot_t * f = table_find(&g_files, path, plen, hash_str(path, plen));
    bool ok = f && f->state == ST_PRESENT && !f->is_dir;
    if (ok)
        *size_out = f->size;
    lazy_lwmutex_unlock(&g_lock);
    return ok;
}

void iocache_emit_report(void) {
    if (!lazy_lwmutex_lock(&g_lock, "asset_index"))
        return;
    report_t r = take_report(true);
    lazy_lwmutex_unlock(&g_lock);
    emit_report(&r);
}

void iocache_invalidate(const char * path) {
    if (!path || !is_asset_path(path))
        return;
    size_t plen = strlen(path);
    size_t dlen = dir_len(path);
    if (plen >= MAX_PATH_LEN || dlen == 0)
        return;
    if (!lazy_lwmutex_lock(&g_lock, "asset_index"))
        return;
    table_erase(&g_dirs, path, dlen, hash_str(path, dlen));
    table_erase(&g_files, path, plen, hash_str(path, plen));
    lazy_lwmutex_unlock(&g_lock);
}
