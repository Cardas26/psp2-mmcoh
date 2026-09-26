#include "reimpl/inflate_fast.h"

#include <stdint.h>
#include <string.h>
#include <libdeflate.h>

#include "utils/lazy_lwmutex.h"
#include "utils/logger.h"

typedef enum { WRAP_ZLIB = 0, WRAP_RAW, WRAP_UNSUPPORTED } wrap_t;

typedef struct {
    z_streamp strm;
    wrap_t    wrap;
    uint8_t   tried;
    uint8_t   done;
} slot_t;

#define SLOTS 16
static slot_t g_slots[SLOTS];
static lazy_lwmutex_t g_slots_lock = LAZY_LWMUTEX_INITIALIZER;

#define POOL 4
static struct libdeflate_decompressor * g_pool[POOL];
static uint8_t g_pool_busy[POOL];
static lazy_lwmutex_t g_pool_lock = LAZY_LWMUTEX_INITIALIZER;

static slot_t * slot_find_locked(z_streamp strm) {
    for (int i = 0; i < SLOTS; i++)
        if (g_slots[i].strm == strm)
            return &g_slots[i];
    return NULL;
}

static void slot_set(z_streamp strm, wrap_t wrap) {
    if (!lazy_lwmutex_lock(&g_slots_lock, "inflate_fast_slots"))
        return;
    slot_t * s = slot_find_locked(strm);
    if (!s)
        s = slot_find_locked(NULL);
    if (s) {
        s->strm = strm;
        s->wrap = wrap;
        s->tried = 0;
        s->done = 0;
    }
    lazy_lwmutex_unlock(&g_slots_lock);
}

static wrap_t wrap_of(int windowBits) {
    if (windowBits < 0)
        return WRAP_RAW;
    if (windowBits >= 16)
        return WRAP_UNSUPPORTED;
    return WRAP_ZLIB;
}

static struct libdeflate_decompressor * pool_acquire(int * idx) {
    if (!lazy_lwmutex_lock(&g_pool_lock, "inflate_fast_pool"))
        return NULL;
    struct libdeflate_decompressor * d = NULL;
    for (int i = 0; i < POOL; i++) {
        if (!g_pool_busy[i]) {
            if (!g_pool[i])
                g_pool[i] = libdeflate_alloc_decompressor();
            if (g_pool[i]) {
                g_pool_busy[i] = 1;
                d = g_pool[i];
                *idx = i;
            }
            break;
        }
    }
    lazy_lwmutex_unlock(&g_pool_lock);
    return d;
}

static void pool_release(int idx) {
    if (!lazy_lwmutex_lock(&g_pool_lock, "inflate_fast_pool"))
        return;
    g_pool_busy[idx] = 0;
    lazy_lwmutex_unlock(&g_pool_lock);
}

int inflateInit_oneshot(z_streamp strm, const char * version, int stream_size) {
    int r = inflateInit_(strm, version, stream_size);
    if (r == Z_OK)
        slot_set(strm, WRAP_ZLIB);
    return r;
}

int inflateInit2_oneshot(z_streamp strm, int windowBits, const char * version,
                         int stream_size) {
    int r = inflateInit2_(strm, windowBits, version, stream_size);
    if (r == Z_OK)
        slot_set(strm, wrap_of(windowBits));
    return r;
}

int inflateReset_oneshot(z_streamp strm) {
    int r = inflateReset(strm);
    if (r == Z_OK && lazy_lwmutex_lock(&g_slots_lock, "inflate_fast_slots")) {
        slot_t * s = slot_find_locked(strm);
        if (s) { s->tried = 0; s->done = 0; }
        lazy_lwmutex_unlock(&g_slots_lock);
    }
    return r;
}

int inflateReset2_oneshot(z_streamp strm, int windowBits) {
    int r = inflateReset2(strm, windowBits);
    if (r == Z_OK)
        slot_set(strm, wrap_of(windowBits));
    return r;
}

int inflateEnd_oneshot(z_streamp strm) {
    if (lazy_lwmutex_lock(&g_slots_lock, "inflate_fast_slots")) {
        slot_t * s = slot_find_locked(strm);
        if (s)
            memset(s, 0, sizeof(*s));
        lazy_lwmutex_unlock(&g_slots_lock);
    }
    return inflateEnd(strm);
}

int inflate_oneshot(z_streamp strm, int flush) {
    if (!strm)
        return inflate(strm, flush);

    wrap_t wrap = WRAP_UNSUPPORTED;
    int eligible = 0, answered = 0;
    if (lazy_lwmutex_lock(&g_slots_lock, "inflate_fast_slots")) {
        const slot_t * s = slot_find_locked(strm);
        if (s) {
            if (s->done)
                answered = 1;
            else if (!s->tried) {
                wrap = s->wrap;
                eligible = 1;
            }
        }
        lazy_lwmutex_unlock(&g_slots_lock);
    }

    if (answered)
        return Z_STREAM_END;

    if (!eligible || wrap == WRAP_UNSUPPORTED)
        return inflate(strm, flush);
    if (strm->total_in != 0 || strm->total_out != 0)
        return inflate(strm, flush);
    if (!strm->next_in || !strm->next_out || !strm->avail_in || !strm->avail_out)
        return inflate(strm, flush);

    int idx = -1;
    struct libdeflate_decompressor * d = pool_acquire(&idx);
    if (!d)
        return inflate(strm, flush);

    const uint8_t * in = (const uint8_t *)strm->next_in;
    size_t in_used = 0, out_used = 0;
    enum libdeflate_result r =
        (wrap == WRAP_ZLIB)
            ? libdeflate_zlib_decompress_ex(d, in, strm->avail_in,
                                            strm->next_out, strm->avail_out,
                                            &in_used, &out_used)
            : libdeflate_deflate_decompress_ex(d, in, strm->avail_in,
                                               strm->next_out, strm->avail_out,
                                               &in_used, &out_used);
    pool_release(idx);

    if (lazy_lwmutex_lock(&g_slots_lock, "inflate_fast_slots")) {
        slot_t * s = slot_find_locked(strm);
        if (s) {
            s->tried = 1;
            s->done = (r == LIBDEFLATE_SUCCESS);
        }
        lazy_lwmutex_unlock(&g_slots_lock);
    }

    if (r != LIBDEFLATE_SUCCESS) {
        return inflate(strm, flush);
    }

    strm->next_in   += in_used;
    strm->avail_in  -= (uInt)in_used;
    strm->total_in  += in_used;
    strm->next_out  += out_used;
    strm->avail_out -= (uInt)out_used;
    strm->total_out += out_used;
    strm->msg = NULL;
    if (wrap == WRAP_ZLIB && in_used >= 4) {
        const uint8_t * t = in + in_used - 4;
        strm->adler = ((uLong)t[0] << 24) | ((uLong)t[1] << 16)
                    | ((uLong)t[2] << 8)  | (uLong)t[3];
    }
    return Z_STREAM_END;
}
