#include "reimpl/iocontent.h"

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <psp2/kernel/processmgr.h>

#include "reimpl/io.h"
#include "reimpl/iocache.h"
#include "reimpl/iopath.h"
#include "utils/lazy_lwmutex.h"
#include "utils/logger.h"

static const char * const ASSET_ROOTS[] = { "/DLC/", "/assets/" };

#define MAX_FILE_BYTES   (2u * 1024 * 1024)
#define CACHE_BUDGET     (32u * 1024 * 1024)
#define MAX_OPEN_FDS     64

typedef struct entry {
    struct entry * next;
    char *    path;
    uint8_t * data;
    uint32_t  size;
    time_t    mtime;
    uint32_t  refs;
    uint64_t  used;
    bool      unverified;
    bool      doomed;
} entry_t;

typedef struct {
    int       fd;
    entry_t * entry;
    uint32_t  pos;
} open_fd_t;

#define BUCKETS 256
static entry_t *  g_buckets[BUCKETS];
static open_fd_t  g_fds[MAX_OPEN_FDS];
static uint32_t   g_bytes;
static uint64_t   g_clock;
static lazy_lwmutex_t g_lock = LAZY_LWMUTEX_INITIALIZER;

static uint32_t g_hit_reads, g_fill, g_evict, g_miss_reads;
static uint64_t g_hit_bytes;
static uint32_t g_since_report;
#define REPORT_EVERY 512

static bool     g_pack_dirty;
static uint64_t g_last_fill_us;
#define pack_note_change() \
    do { g_pack_dirty = true; g_last_fill_us = sceKernelGetProcessTimeWide(); } while (0)

static uint32_t hash_str(const char * s) {
    uint32_t h = 2166136261u;
    for (; *s; s++) { h ^= (uint8_t) *s; h *= 16777619u; }
    return h;
}

static const char * const STDIO_ROOTS[] = { "/SoundBanks/" };

static bool is_asset_path(const char * path) {
    for (size_t i = 0; i < sizeof(STDIO_ROOTS) / sizeof(STDIO_ROOTS[0]); i++)
        if (strstr(path, STDIO_ROOTS[i]))
            return false;
    for (size_t i = 0; i < sizeof(ASSET_ROOTS) / sizeof(ASSET_ROOTS[0]); i++)
        if (strstr(path, ASSET_ROOTS[i]))
            return true;
    return false;
}

static entry_t * find_entry(const char * path, uint32_t h) {
    for (entry_t * e = g_buckets[h % BUCKETS]; e; e = e->next)
        if (strcmp(e->path, path) == 0)
            return e;
    return NULL;
}

static void unlink_entry(entry_t * e) {
    for (entry_t ** link = &g_buckets[hash_str(e->path) % BUCKETS]; *link;
         link = &(*link)->next)
        if (*link == e) {
            *link = e->next;
            break;
        }
    g_bytes -= e->size;
    free(e->data);
    free(e->path);
    free(e);
}

static void drop_entry(entry_t * e) {
    if (e->refs == 0)
        unlink_entry(e);
    else
        e->doomed = true;
}

static void entry_unref(entry_t * e) {
    if (e->refs)
        e->refs--;
    if (e->refs == 0 && e->doomed)
        unlink_entry(e);
}

static bool asset_stat(const char * path, int fd, uint32_t * size, time_t * mtime) {
    struct stat st;
    int res;
    if (!(iocache_stat(path, &st, &res) && res == 0)) {
        if (fd < 0 || fstat(fd, &st) != 0)
            return false;
    }
    if (!S_ISREG(st.st_mode))
        return false;
    *size = (uint32_t) st.st_size;
    *mtime = st.st_mtime;
    return true;
}

static bool verify_entry(entry_t * e, uint32_t size, time_t mtime) {
    if (e->size != size || e->mtime != mtime)
        return false;
    e->unverified = false;
    return true;
}

static void make_room(uint32_t need) {
    while (g_bytes + need > CACHE_BUDGET) {
        entry_t * worst = NULL, ** worst_link = NULL;
        for (int b = 0; b < BUCKETS; b++)
            for (entry_t ** link = &g_buckets[b]; *link; link = &(*link)->next)
                if ((*link)->refs == 0 && (!worst || (*link)->used < worst->used)) {
                    worst = *link;
                    worst_link = link;
                }
        if (!worst)
            return;
        *worst_link = worst->next;
        g_bytes -= worst->size;
        g_evict++;
        free(worst->data);
        free(worst->path);
        free(worst);
    }
}

static bool g_fds_ready;

static open_fd_t * find_fd(int fd) {
    if (!g_fds_ready) {
        for (int i = 0; i < MAX_OPEN_FDS; i++)
            g_fds[i].fd = -1;
        g_fds_ready = true;
    }
    for (int i = 0; i < MAX_OPEN_FDS; i++)
        if (g_fds[i].fd == fd)
            return &g_fds[i];
    return NULL;
}

typedef struct {
    bool due;
    uint32_t bytes_kb, fill, evict, hit, miss;
} report_t;

static report_t take_report(void) {
    report_t r = {0};
    return r;
}

static uint32_t g_handle_hits, g_handle_pool_full;

static void emit_report(const report_t * r) {
    (void) r;
}

void iocontent_open(const char * path, int fd) {
    if (fd < 0 || !path || !is_asset_path(path))
        return;

    uint32_t size;
    time_t mtime;
    if (!asset_stat(path, fd, &size, &mtime))
        return;
    if (size == 0 || size > MAX_FILE_BYTES)
        return;

    if (!lazy_lwmutex_lock(&g_lock, "asset_content"))
        return;

    uint32_t h = hash_str(path);
    entry_t * e = find_entry(path, h);

    if (e && e->unverified && !verify_entry(e, size, mtime)) {
        drop_entry(e);
        pack_note_change();
        e = NULL;
    }

    if (!e) {
        make_room(size);
        if (g_bytes + size > CACHE_BUDGET) {
            lazy_lwmutex_unlock(&g_lock);
            return;
        }
        uint8_t * data = malloc(size);
        char * copy = data ? strdup(path) : NULL;
        if (!data || !copy) {
            free(data);
            free(copy);
            lazy_lwmutex_unlock(&g_lock);
            return;
        }
        uint32_t got = 0;
        while (got < size) {
            ssize_t n = io_read_timed(fd, data + got, size - got);
            if (n <= 0)
                break;
            got += (uint32_t) n;
        }
        io_lseek_timed(fd, 0, SEEK_SET);
        if (got != size) {
            free(data);
            free(copy);
            lazy_lwmutex_unlock(&g_lock);
            return;
        }

        e = malloc(sizeof(entry_t));
        if (!e) {
            free(data);
            free(copy);
            lazy_lwmutex_unlock(&g_lock);
            return;
        }
        e->path = copy;
        e->data = data;
        e->size = size;
        e->mtime = mtime;
        e->refs = 0;
        e->unverified = false;
        e->doomed = false;
        e->next = g_buckets[h % BUCKETS];
        g_buckets[h % BUCKETS] = e;
        g_bytes += size;
        g_fill++;
        pack_note_change();
    }

    open_fd_t * slot = find_fd(-1);
    if (slot) {
        slot->fd = fd;
        slot->entry = e;
        slot->pos = 0;
        e->refs++;
        e->used = ++g_clock;
    }
    lazy_lwmutex_unlock(&g_lock);
}

bool iocontent_read(int fd, void * buf, size_t count, ssize_t * ret) {
    if (fd < 0)
        return false;
    if (!lazy_lwmutex_lock(&g_lock, "asset_content"))
        return false;

    open_fd_t * slot = find_fd(fd);
    if (!slot) {
        g_miss_reads++;
        report_t r = take_report();
        lazy_lwmutex_unlock(&g_lock);
        emit_report(&r);
        return false;
    }

    entry_t * e = slot->entry;
    uint32_t left = e->size - slot->pos;
    uint32_t n = count < left ? (uint32_t) count : left;
    if (n)
        memcpy(buf, e->data + slot->pos, n);
    slot->pos += n;
    e->used = ++g_clock;
    g_hit_reads++;
    g_hit_bytes += n;

    report_t r = take_report();
    lazy_lwmutex_unlock(&g_lock);
    emit_report(&r);
    *ret = (ssize_t) n;
    return true;
}

bool iocontent_seek(int fd, off_t offset, int whence, off_t * ret) {
    if (fd < 0)
        return false;
    if (!lazy_lwmutex_lock(&g_lock, "asset_content"))
        return false;

    open_fd_t * slot = find_fd(fd);
    if (!slot) {
        lazy_lwmutex_unlock(&g_lock);
        return false;
    }

    int64_t base = (whence == SEEK_CUR) ? slot->pos
                 : (whence == SEEK_END) ? slot->entry->size : 0;
    int64_t want = base + offset;
    if (want < 0 || want > (int64_t) slot->entry->size) {
        lazy_lwmutex_unlock(&g_lock);
        errno = EINVAL;
        *ret = (off_t) -1;
        return true;
    }
    slot->pos = (uint32_t) want;
    lazy_lwmutex_unlock(&g_lock);
    *ret = (off_t) want;
    return true;
}

void iocontent_close(int fd) {
    if (fd < 0)
        return;
    if (!lazy_lwmutex_lock(&g_lock, "asset_content"))
        return;
    open_fd_t * slot = find_fd(fd);
    if (slot) {
        entry_unref(slot->entry);
        slot->fd = -1;
        slot->entry = NULL;
    }
    lazy_lwmutex_unlock(&g_lock);
}

#define HANDLE_POOL     8
#define SENTINEL_PATH   "app0:/empty"

typedef struct {
    FILE * f;
    int    fd;
    bool   busy;
} handle_t;

static handle_t g_pool[HANDLE_POOL];
static bool     g_pool_ready;
static void pool_init(void) {
    for (int i = 0; i < HANDLE_POOL; i++) {
        g_pool[i].f = fopen(SENTINEL_PATH, "rb");
        g_pool[i].fd = g_pool[i].f ? fileno(g_pool[i].f) : -1;
        g_pool[i].busy = false;
        if (g_pool[i].fd < 0)
            l_warn("[assethandle] no " SENTINEL_PATH ": slot %d unusable", i);
    }
    g_pool_ready = true;
}

FILE * iocontent_take_handle(const char * path, const char * mode) {
    if (!path || !mode || mode[0] != 'r' || strchr(mode, '+') ||
        !is_asset_path(path))
        return NULL;

    if (!lazy_lwmutex_lock(&g_lock, "asset_content"))
        return NULL;

    entry_t * e = find_entry(path, hash_str(path));

    if (e && e->unverified) {
        uint32_t size;
        time_t mtime;
        if (!asset_stat(path, -1, &size, &mtime)) {
            e = NULL;
        } else if (!verify_entry(e, size, mtime)) {
            drop_entry(e);
            pack_note_change();
            e = NULL;
        }
    }
    if (!e) {
        lazy_lwmutex_unlock(&g_lock);
        return NULL;
    }

    if (!g_pool_ready)
        pool_init();

    handle_t * h = NULL;
    for (int i = 0; i < HANDLE_POOL; i++)
        if (!g_pool[i].busy && g_pool[i].fd >= 0) {
            h = &g_pool[i];
            break;
        }
    open_fd_t * slot = h ? find_fd(-1) : NULL;
    if (!slot) {
        g_handle_pool_full++;
        lazy_lwmutex_unlock(&g_lock);
        return NULL;
    }

    h->busy = true;
    slot->fd = h->fd;
    slot->entry = e;
    slot->pos = 0;
    e->refs++;
    e->used = ++g_clock;
    g_handle_hits++;
    lazy_lwmutex_unlock(&g_lock);
    return h->f;
}

bool iocontent_give_handle(FILE * f) {
    if (!f || !g_pool_ready)
        return false;
    if (!lazy_lwmutex_lock(&g_lock, "asset_content"))
        return false;

    bool ours = false;
    for (int i = 0; i < HANDLE_POOL; i++) {
        if (g_pool[i].f != f || !g_pool[i].busy)
            continue;
        open_fd_t * slot = find_fd(g_pool[i].fd);
        if (slot) {
            if (slot->entry)
                entry_unref(slot->entry);
            slot->fd = -1;
            slot->entry = NULL;
        }
        g_pool[i].busy = false;
        ours = true;
        break;
    }
    lazy_lwmutex_unlock(&g_lock);
    return ours;
}

void iocontent_invalidate(const char * path) {
    if (!path || !is_asset_path(path))
        return;
    if (!lazy_lwmutex_lock(&g_lock, "asset_content"))
        return;
    entry_t * e = find_entry(path, hash_str(path));
    if (e) {
        drop_entry(e);
        pack_note_change();
    }
    lazy_lwmutex_unlock(&g_lock);
}

#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/threadmgr.h>

#define PACK_PATH          DATA_PATH "assetpack.bin"
#define PACK_TMP_PATH      DATA_PATH "assetpack.tmp"
#define PACK_MAGIC         0x4b504843u
#define PACK_VERSION       1u
#define PACK_HEADER_BYTES  24u
#define PACK_RECORD_BYTES  14u
#define PACK_MAX_FILES     8192u
#define PACK_MAX_TABLE     (4u * 1024 * 1024)
#define PACK_READ_CHUNK    (1u * 1024 * 1024)
#define PACK_WRITE_CHUNK   (256u * 1024)
#define PACK_POLL_US       (5ull * 1000 * 1000)
#define PACK_QUIET_US      (30ull * 1000 * 1000)
#define PACK_THREAD_PRIORITY 0x10000100
#define PACK_THREAD_STACK    (64 * 1024)

#define PACK_DELTA_PATH    DATA_PATH "assetpack.add"
#define PACK_DELTA_MAGIC   0x41504843u
#define PACK_DELTA_REC     18u
#define PACK_FOLD_BYTES    (24u * 1024 * 1024)
#define CARD_SLOTS         4096u
#define CARD_MAX           (CARD_SLOTS * 3u / 4u)
#define CARD_FOLD_COUNT    2048u

typedef struct { uint32_t hash, size; int64_t mtime; } card_key_t;
static card_key_t g_card[CARD_SLOTS];
static uint32_t   g_card_n;
static uint32_t   g_delta_end;

static uint32_t card_hash(const char * path) {
    uint32_t h = hash_str(path);
    return h ? h : 1u;
}

static bool card_has(uint32_t h, uint32_t size, time_t mtime) {
    for (uint32_t i = h % CARD_SLOTS, n = 0; n < CARD_SLOTS && g_card[i].hash;
         i = (i + 1) % CARD_SLOTS, n++)
        if (g_card[i].hash == h && g_card[i].size == size &&
            g_card[i].mtime == (int64_t) mtime)
            return true;
    return false;
}

static void card_add(uint32_t h, uint32_t size, time_t mtime) {
    if (g_card_n >= CARD_MAX || card_has(h, size, mtime))
        return;
    uint32_t i = h % CARD_SLOTS;
    while (g_card[i].hash)
        i = (i + 1) % CARD_SLOTS;
    g_card[i].hash = h;
    g_card[i].size = size;
    g_card[i].mtime = (int64_t) mtime;
    g_card_n++;
}

typedef struct {
    SceUID    fd;
    uint8_t * buf;
    uint32_t  len, pos;
} pack_reader_t;

static bool pack_fill(pack_reader_t * r, uint8_t * dst, uint32_t n) {
    while (n) {
        if (r->pos == r->len) {
            int got = sceIoRead(r->fd, r->buf, PACK_READ_CHUNK);
            if (got <= 0)
                return false;
            r->len = (uint32_t) got;
            r->pos = 0;
        }
        uint32_t take = r->len - r->pos;
        if (take > n)
            take = n;
        if (dst) {
            memcpy(dst, r->buf + r->pos, take);
            dst += take;
        }
        r->pos += take;
        n -= take;
    }
    return true;
}

static uint32_t rd_u32(const uint8_t * p) { uint32_t v; memcpy(&v, p, 4); return v; }
static uint64_t rd_u64(const uint8_t * p) { uint64_t v; memcpy(&v, p, 8); return v; }
static uint16_t rd_u16(const uint8_t * p) { uint16_t v; memcpy(&v, p, 2); return v; }
static void wr_u32(uint8_t * p, uint32_t v) { memcpy(p, &v, 4); }
static void wr_u64(uint8_t * p, uint64_t v) { memcpy(p, &v, 8); }
static void wr_u16(uint8_t * p, uint16_t v) { memcpy(p, &v, 2); }

static bool pack_adopt(const char * path, uint8_t * data, uint32_t size, time_t mtime) {
    if (!lazy_lwmutex_lock(&g_lock, "asset_content"))
        return false;
    uint32_t h = hash_str(path);
    bool ok = false;
    if (!find_entry(path, h) && g_bytes + size <= CACHE_BUDGET) {
        char * copy = strdup(path);
        entry_t * e = copy ? malloc(sizeof(entry_t)) : NULL;
        if (e) {
            e->path = copy;
            e->data = data;
            e->size = size;
            e->mtime = mtime;
            e->refs = 0;
            e->used = ++g_clock;
            e->unverified = true;
            e->doomed = false;
            e->next = g_buckets[h % BUCKETS];
            g_buckets[h % BUCKETS] = e;
            g_bytes += size;
            ok = true;
        } else {
            free(copy);
        }
    }
    lazy_lwmutex_unlock(&g_lock);
    return ok;
}

typedef struct {
    const char * status;
    uint32_t files, kb, ms;
    uint32_t file_kb;
} pack_stats_t;

static void pack_load(pack_stats_t * s) {
    uint64_t t0 = sceKernelGetProcessTimeWide();
    s->status = "none";
    SceUID fd = sceIoOpen(PACK_PATH, SCE_O_RDONLY, 0);
    if (fd < 0)
        return;
    SceOff fsize = sceIoLseek(fd, 0, SCE_SEEK_END);
    sceIoLseek(fd, 0, SCE_SEEK_SET);

    pack_reader_t r = { fd, malloc(PACK_READ_CHUNK), 0, 0 };
    uint8_t * table = NULL;
    s->status = "corrupt";
    uint8_t hdr[PACK_HEADER_BYTES];
    if (!r.buf) {
        s->status = "nomem";
        goto out;
    }
    if (!pack_fill(&r, hdr, sizeof(hdr)))
        goto out;
    uint32_t count = rd_u32(hdr + 8), table_bytes = rd_u32(hdr + 12);
    if (rd_u32(hdr) != PACK_MAGIC || rd_u32(hdr + 4) != PACK_VERSION ||
        count > PACK_MAX_FILES || table_bytes > PACK_MAX_TABLE ||
        fsize < 0 || rd_u64(hdr + 16) != (uint64_t) fsize)
        goto out;
    table = malloc(table_bytes);
    if (!table) {
        s->status = "nomem";
        goto out;
    }
    if (!pack_fill(&r, table, table_bytes))
        goto out;

    char path[SOLOADER_PATH_MAX];
    uint32_t off = 0;
    for (uint32_t i = 0; i < count; i++) {
        if (off + PACK_RECORD_BYTES > table_bytes)
            goto out;
        uint32_t size = rd_u32(table + off);
        time_t mtime = (time_t) rd_u64(table + off + 4);
        uint16_t plen = rd_u16(table + off + 12);
        off += PACK_RECORD_BYTES;
        if (plen == 0 || plen >= sizeof(path) || off + plen > table_bytes)
            goto out;
        memcpy(path, table + off, plen);
        path[plen] = 0;
        off += plen;

        if (size == 0 || size > MAX_FILE_BYTES || !is_asset_path(path)) {
            if (!pack_fill(&r, NULL, size))
                goto out;
            continue;
        }
        uint8_t * data = malloc(size);
        if (!data) {
            s->status = "nomem";
            goto out;
        }
        if (!pack_fill(&r, data, size)) {
            free(data);
            goto out;
        }
        card_add(card_hash(path), size, mtime);
        if (pack_adopt(path, data, size, mtime)) {
            s->files++;
            s->kb += size / 1024;
        } else {
            free(data);
        }
    }
    s->status = "ok";
out:
    free(table);
    free(r.buf);
    sceIoClose(fd);
    s->ms = (uint32_t)((sceKernelGetProcessTimeWide() - t0) / 1000);
}

static bool write_all(SceUID fd, const uint8_t * p, uint32_t n) {
    while (n) {
        int w = sceIoWrite(fd, p, n);
        if (w <= 0)
            return false;
        p += w;
        n -= (uint32_t) w;
    }
    return true;
}

static void pack_write(pack_stats_t * s) {
    uint64_t t0 = sceKernelGetProcessTimeWide();
    s->status = "nomem";

    if (!lazy_lwmutex_lock(&g_lock, "asset_content"))
        return;
    uint32_t n = 0, table_bytes = 0;
    uint64_t total = PACK_HEADER_BYTES;
    for (int b = 0; b < BUCKETS; b++)
        for (entry_t * e = g_buckets[b]; e; e = e->next)
            if (!e->doomed)
                n++;
    entry_t ** list = malloc((n ? n : 1) * sizeof(entry_t *));
    if (!list) {
        lazy_lwmutex_unlock(&g_lock);
        return;
    }
    uint32_t i = 0;
    for (int b = 0; b < BUCKETS; b++)
        for (entry_t * e = g_buckets[b]; e; e = e->next) {
            if (e->doomed)
                continue;
            list[i++] = e;
            e->refs++;
            table_bytes += PACK_RECORD_BYTES + (uint32_t) strlen(e->path);
            total += e->size;
        }
    lazy_lwmutex_unlock(&g_lock);
    total += table_bytes;

    uint8_t * table = malloc(table_bytes);
    bool ok = table != NULL;
    if (ok) {
        uint32_t off = 0;
        for (i = 0; i < n; i++) {
            const entry_t * e = list[i];
            uint16_t plen = (uint16_t) strlen(e->path);
            wr_u32(table + off, e->size);
            wr_u64(table + off + 4, (uint64_t) e->mtime);
            wr_u16(table + off + 12, plen);
            memcpy(table + off + PACK_RECORD_BYTES, e->path, plen);
            off += PACK_RECORD_BYTES + plen;
        }
        uint8_t hdr[PACK_HEADER_BYTES];
        wr_u32(hdr, PACK_MAGIC);
        wr_u32(hdr + 4, PACK_VERSION);
        wr_u32(hdr + 8, n);
        wr_u32(hdr + 12, table_bytes);
        wr_u64(hdr + 16, total);

        s->status = "io";
        SceUID fd = sceIoOpen(PACK_TMP_PATH, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
        ok = fd >= 0 && write_all(fd, hdr, sizeof(hdr)) && write_all(fd, table, table_bytes);
        for (i = 0; ok && i < n; i++) {
            const entry_t * e = list[i];
            for (uint32_t off2 = 0; ok && off2 < e->size; off2 += PACK_WRITE_CHUNK) {
                uint32_t len = e->size - off2;
                if (len > PACK_WRITE_CHUNK)
                    len = PACK_WRITE_CHUNK;
                ok = write_all(fd, e->data + off2, len);
            }
        }
        if (fd >= 0)
            sceIoClose(fd);
        if (ok) {
            sceIoRemove(PACK_PATH);
            ok = sceIoRename(PACK_TMP_PATH, PACK_PATH) >= 0;
        } else {
            sceIoRemove(PACK_TMP_PATH);
        }
        if (ok) {
            memset(g_card, 0, sizeof(g_card));
            g_card_n = 0;
            for (i = 0; i < n; i++)
                card_add(card_hash(list[i]->path), list[i]->size, list[i]->mtime);
            s->status = "ok";
            s->files = n;
            s->kb = (uint32_t)((total - PACK_HEADER_BYTES - table_bytes) / 1024);
        }
    }

    if (lazy_lwmutex_lock(&g_lock, "asset_content")) {
        for (i = 0; i < n; i++)
            entry_unref(list[i]);
        lazy_lwmutex_unlock(&g_lock);
    }
    free(table);
    free(list);
    s->ms = (uint32_t)((sceKernelGetProcessTimeWide() - t0) / 1000);
}

static void delta_load(pack_stats_t * s, bool * torn) {
    uint64_t t0 = sceKernelGetProcessTimeWide();
    s->status = "none";
    *torn = false;
    SceUID fd = sceIoOpen(PACK_DELTA_PATH, SCE_O_RDONLY, 0);
    if (fd < 0)
        return;
    SceOff fsize = sceIoLseek(fd, 0, SCE_SEEK_END);
    sceIoLseek(fd, 0, SCE_SEEK_SET);
    s->file_kb = fsize > 0 ? (uint32_t)(fsize / 1024) : 0;

    pack_reader_t r = { fd, malloc(PACK_READ_CHUNK), 0, 0 };
    char path[SOLOADER_PATH_MAX];
    uint64_t off = 0, good = 0;
    if (!r.buf) {
        s->status = "nomem";
        goto out;
    }
    while (fsize > 0 && off < (uint64_t) fsize) {
        uint8_t rec[PACK_DELTA_REC];
        if (off + PACK_DELTA_REC > (uint64_t) fsize || !pack_fill(&r, rec, sizeof(rec)))
            goto torn_out;
        uint32_t size = rd_u32(rec + 4);
        time_t mtime = (time_t) rd_u64(rec + 8);
        uint16_t plen = rd_u16(rec + 16);
        if (rd_u32(rec) != PACK_DELTA_MAGIC || plen == 0 || plen >= sizeof(path) ||
            size == 0 || size > MAX_FILE_BYTES ||
            off + PACK_DELTA_REC + plen + size > (uint64_t) fsize ||
            !pack_fill(&r, (uint8_t *) path, plen))
            goto torn_out;
        path[plen] = 0;
        off += PACK_DELTA_REC + plen + size;
        if (!is_asset_path(path)) {
            if (!pack_fill(&r, NULL, size))
                goto torn_out;
            good = off;
            continue;
        }
        uint8_t * data = malloc(size);
        if (!data) {
            s->status = "nomem";
            goto out;
        }
        if (!pack_fill(&r, data, size)) {
            free(data);
            goto torn_out;
        }
        card_add(card_hash(path), size, mtime);
        good = off;
        if (pack_adopt(path, data, size, mtime)) {
            s->files++;
            s->kb += size / 1024;
        } else {
            free(data);
        }
    }
    s->status = "ok";
    goto out;
torn_out:
    *torn = true;
    s->status = "torn";
out:
    g_delta_end = (uint32_t) good;
    free(r.buf);
    sceIoClose(fd);
    s->ms = (uint32_t)((sceKernelGetProcessTimeWide() - t0) / 1000);
}

static void pack_append(pack_stats_t * s) {
    uint64_t t0 = sceKernelGetProcessTimeWide();
    s->status = "nomem";
    if (!lazy_lwmutex_lock(&g_lock, "asset_content"))
        return;
    uint32_t n = 0;
    for (int b = 0; b < BUCKETS; b++)
        for (entry_t * e = g_buckets[b]; e; e = e->next)
            if (!e->doomed && !e->unverified &&
                !card_has(card_hash(e->path), e->size, e->mtime))
                n++;
    if (n == 0) {
        lazy_lwmutex_unlock(&g_lock);
        s->status = "none";
        return;
    }
    entry_t ** list = malloc(n * sizeof(entry_t *));
    if (!list) {
        lazy_lwmutex_unlock(&g_lock);
        return;
    }
    uint32_t i = 0;
    for (int b = 0; b < BUCKETS && i < n; b++)
        for (entry_t * e = g_buckets[b]; e && i < n; e = e->next)
            if (!e->doomed && !e->unverified &&
                !card_has(card_hash(e->path), e->size, e->mtime)) {
                list[i++] = e;
                e->refs++;
            }
    lazy_lwmutex_unlock(&g_lock);

    s->status = "io";
    SceUID fd = sceIoOpen(PACK_DELTA_PATH, SCE_O_WRONLY | SCE_O_CREAT, 0777);
    bool ok = fd >= 0 &&
              sceIoLseek(fd, (SceOff) g_delta_end, SCE_SEEK_SET) == (SceOff) g_delta_end;
    for (i = 0; ok && i < n; i++) {
        const entry_t * e = list[i];
        uint16_t plen = (uint16_t) strlen(e->path);
        uint8_t rec[PACK_DELTA_REC];
        wr_u32(rec, PACK_DELTA_MAGIC);
        wr_u32(rec + 4, e->size);
        wr_u64(rec + 8, (uint64_t) e->mtime);
        wr_u16(rec + 16, plen);
        ok = write_all(fd, rec, sizeof(rec)) &&
             write_all(fd, (const uint8_t *) e->path, plen) &&
             write_all(fd, e->data, e->size);
        if (ok) {
            card_add(card_hash(e->path), e->size, e->mtime);
            g_delta_end += PACK_DELTA_REC + plen + e->size;
            s->files++;
            s->kb += e->size / 1024;
        }
    }
    if (fd >= 0)
        sceIoClose(fd);
    if (ok)
        s->status = "ok";

    if (lazy_lwmutex_lock(&g_lock, "asset_content")) {
        for (i = 0; i < n; i++)
            entry_unref(list[i]);
        lazy_lwmutex_unlock(&g_lock);
    }
    free(list);
    s->ms = (uint32_t)((sceKernelGetProcessTimeWide() - t0) / 1000);
}

static int pack_thread(SceSize args, void * argp) {
    (void) args;
    (void) argp;
    pack_stats_t s = {0};
    pack_stats_t d = {0};
    bool torn = false;
    delta_load(&d, &torn);
    l_perf("[assetpack] delta load %s: %u files, %u KB adopted of a %u KB file, %u KB good%s,"
           " in %u ms", d.status, d.files, d.kb, d.file_kb, g_delta_end / 1024u,
           torn ? " (the next append writes over the rest)" : "", d.ms);
    pack_load(&s);
    l_perf("[assetpack] load %s: %u files, %u KB in %u ms", s.status, s.files, s.kb, s.ms);
    bool pack_bad = strcmp(s.status, "ok") != 0 && strcmp(s.status, "none") != 0;
    if (pack_bad || g_delta_end > PACK_FOLD_BYTES || g_card_n > CARD_FOLD_COUNT) {
        uint32_t good_kb = g_delta_end / 1024u;
        pack_stats_t w = {0};
        pack_write(&w);
        if (strcmp(w.status, "ok") == 0) {
            sceIoRemove(PACK_DELTA_PATH);
            g_delta_end = 0;
        }
        l_perf("[assetpack] fold %s: %u files, %u KB in %u ms (delta %u KB, %u good)",
               w.status, w.files, w.kb, w.ms, d.file_kb, good_kb);
    }

    for (;;) {
        sceKernelDelayThread(PACK_POLL_US);
        bool due = false;
        if (lazy_lwmutex_lock(&g_lock, "asset_content")) {
            due = g_pack_dirty &&
                  sceKernelGetProcessTimeWide() - g_last_fill_us >= PACK_QUIET_US;
            if (due)
                g_pack_dirty = false;
            lazy_lwmutex_unlock(&g_lock);
        }
        if (!due)
            continue;
        pack_stats_t w = {0};
        if (g_card_n >= CARD_MAX)
            continue;
        pack_append(&w);
        if (strcmp(w.status, "none") != 0)
            l_perf("[assetpack] append %s: %u files, %u KB in %u ms", w.status,
                   w.files, w.kb, w.ms);
    }
    return 0;
}

void iocontent_pack_start(void) {
    SceUID t = sceKernelCreateThread("soloader_assetpack", pack_thread,
                                     PACK_THREAD_PRIORITY, PACK_THREAD_STACK, 0, 0, NULL);
    if (t < 0) {
        l_warn("[assetpack] could not create thread: 0x%08X - cold session", t);
        return;
    }
    int ret = sceKernelStartThread(t, 0, NULL);
    if (ret < 0) {
        l_warn("[assetpack] could not start thread: 0x%08X - cold session", ret);
        sceKernelDeleteThread(t);
    }
}
