#include "audio/music_seg_cache.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <psp2/io/fcntl.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>

#include "audio/music_bank_map.h"
#include "audio/music_cue_map.h"
#include "utils/lazy_lwmutex.h"
#include "utils/logger.h"

#define SLOTS        8
#define SLOT_MAX     (4u << 20)
#define CACHE_MAX    (12u << 20)
#define WANT_MAX     4
#define READ_CHUNK   (256 * 1024)
#define HANDLES      4
#define WAIT_MAX_US  3000000

enum { S_EMPTY = 0, S_LOADING, S_READY };

struct slot {
	int            bank;
	unsigned char *buf;
	unsigned int   off;
	unsigned int   cap;
	unsigned int   len;
	int            state;
	uint32_t       gen;
	uint64_t       used;
};

static struct slot   g_slot[SLOTS];
static struct slot   g_hdr;
static lazy_lwmutex_t g_lock = LAZY_LWMUTEX_INITIALIZER;
static FILE         *g_handle[HANDLES];
static char          g_path[512];
static int           g_want[WANT_MAX];
static int           g_want_n;
static int           g_queue[WANT_MAX];
static int           g_queue_n;
static SceUID        g_flag = -1;
static SceUID        g_thread = -1;
static int           g_hdr_wanted = 1;
static uint32_t      g_fopen_count;
static uint32_t      g_hint_fopen;

#define LOCK()   lazy_lwmutex_lock(&g_lock, "music_seg_cache")
#define UNLOCK() lazy_lwmutex_unlock(&g_lock)

static uint64_t now_us(void) { return sceKernelGetProcessTimeWide(); }

static const char *bank_name(int bank) {
	return (bank >= 0 && bank < MUSIC_BANK_MAP_N) ? MUSIC_BANK_MAP[bank].name : "(header)";
}

#define FMOD_BLOCK 2048u
static unsigned int seg_lo(int bank) {
	return MUSIC_BANK_MAP[bank].offset & ~(FMOD_BLOCK - 1);
}
static unsigned int seg_hi(int bank) {
	return (MUSIC_BANK_MAP[bank].offset + MUSIC_BANK_MAP[bank].length + FMOD_BLOCK - 1) & ~(FMOD_BLOCK - 1);
}
static int g_cur_bank = -1;

static int seg_holding(long pos, size_t bytes) {
	if (pos < 0) return -1;
	uint64_t p = (uint64_t)pos, e = p + bytes;
	int c = g_cur_bank;
	if (c >= 0 && c < MUSIC_BANK_MAP_N && p >= seg_lo(c) && e <= seg_hi(c)) return c;
	for (int b = 0; b < MUSIC_BANK_MAP_N; b++)
		if (p >= seg_lo(b) && e <= seg_hi(b)) return b;
	return -1;
}

static const char *basename_of(const char *p) {
	const char *s = strrchr(p, '/');
	return s ? s + 1 : p;
}

bool music_seg_cache_is_bank(FILE *f) {
	if (!f) return false;
	for (int i = 0; i < HANDLES; i++)
		if (g_handle[i] == f) return true;
	return false;
}

static int prefetch_thread(SceSize args, void *argp);

void music_seg_cache_bank_fopen(FILE *f, const char *path) {
	if (!f || !path || strcmp(basename_of(path), "MMCoH_Music_Bank.fsb") != 0)
		return;
	if (!LOCK()) return;
	for (int i = 0; i < HANDLES; i++) {
		if (!g_handle[i]) { g_handle[i] = f; break; }
	}
	g_fopen_count++;
	if (!g_path[0]) {
		strncpy(g_path, path, sizeof(g_path) - 1);
		g_hdr.bank = -1;
		g_hdr.off  = 0;
		g_hdr.cap  = (MUSIC_BANK_MAP[0].offset + FMOD_BLOCK - 1) & ~(FMOD_BLOCK - 1);
		g_hdr.state = S_EMPTY;
	}
	int kick = g_path[0] && (g_hdr_wanted || g_queue_n);
	UNLOCK();
	if (kick && g_flag >= 0)
		sceKernelSetEventFlag(g_flag, 1);
}

void music_seg_cache_bank_fclose(FILE *f) {
	if (!f) return;
	for (int i = 0; i < HANDLES; i++)
		if (g_handle[i] == f) g_handle[i] = NULL;
	if (g_queue_n && g_flag >= 0)
		sceKernelSetEventFlag(g_flag, 1);
}

static bool bank_handle_open(void) {
	for (int i = 0; i < HANDLES; i++)
		if (g_handle[i]) return true;
	return false;
}

static bool bank_protected(int bank) {
	if (bank == g_cur_bank) return true;
	for (int i = 0; i < g_want_n; i++)
		if (g_want[i] == bank) return true;
	return false;
}

static struct slot *find_slot(int bank) {
	for (int i = 0; i < SLOTS; i++)
		if (g_slot[i].state != S_EMPTY && g_slot[i].bank == bank) return &g_slot[i];
	return NULL;
}

static unsigned int bytes_held(void) {
	unsigned int n = 0;
	for (int i = 0; i < SLOTS; i++)
		if (g_slot[i].state != S_EMPTY) n += g_slot[i].cap;
	return n;
}

static struct slot *lru_victim(void) {
	struct slot *s = NULL;
	uint64_t oldest = UINT64_MAX;
	for (int i = 0; i < SLOTS; i++) {
		struct slot *c = &g_slot[i];
		if (c->state != S_READY || bank_protected(c->bank)) continue;
		if (c->used < oldest) { oldest = c->used; s = c; }
	}
	return s;
}

static void drop_slot(struct slot *s, int for_bank) {
	l_perf("[music-cache] evict %d %s (%u KB) for %d %s", s->bank, bank_name(s->bank),
	       s->cap / 1024, for_bank, bank_name(for_bank));
	free(s->buf); s->buf = NULL; s->cap = 0; s->len = 0; s->state = S_EMPTY; s->gen++;
}

static struct slot *take_slot(int bank, unsigned int off, unsigned int cap) {
	while (bytes_held() + cap > CACHE_MAX) {
		struct slot *v = lru_victim();
		if (!v) return NULL;
		drop_slot(v, bank);
	}
	struct slot *s = NULL;
	for (int i = 0; i < SLOTS && !s; i++)
		if (g_slot[i].state == S_EMPTY) s = &g_slot[i];
	if (!s) {
		s = lru_victim();
		if (!s) return NULL;
		drop_slot(s, bank);
	}
	if (s->cap != cap || !s->buf) {
		free(s->buf);
		s->buf = malloc(cap);
		if (!s->buf) {
			s->state = S_EMPTY; s->cap = 0;
			l_error("[music-cache] malloc(%u) failed for %d %s", cap, bank, bank_name(bank));
			return NULL;
		}
	}
	s->bank = bank; s->off = off; s->cap = cap; s->len = 0;
	s->state = S_LOADING; s->gen++; s->used = now_us();
	return s;
}

static struct slot *slot_covering(long pos, size_t bytes) {
	if (pos < 0) return NULL;
	uint64_t p = (uint64_t)pos, e = p + bytes;
	if (g_hdr.state != S_EMPTY && e <= g_hdr.cap) return &g_hdr;
	struct slot *ready = NULL, *loading = NULL;
	for (int i = 0; i < SLOTS; i++) {
		struct slot *s = &g_slot[i];
		if (s->state == S_EMPTY) continue;
		if (p < s->off || e > (uint64_t)s->off + s->cap) continue;
		if (s->bank == g_cur_bank && s->state == S_READY) return s;
		if (s->state == S_READY) { if (!ready) ready = s; }
		else if (!loading) loading = s;
	}
	return ready ? ready : loading;
}

static void load_range(struct slot *s, int bank);

size_t music_seg_cache_read(FILE *f, long pos, void *ptr, size_t bytes) {
	if (!bytes || !music_seg_cache_is_bank(f)) return 0;
	if (!LOCK()) return 0;
	struct slot *s = slot_covering(pos, bytes);
	int cold = 0;
	if (!s) {
		int bank = seg_holding(pos, bytes);
		if (bank < 0) {
			UNLOCK();
			l_perf("[music-cache] miss +%ld %u bytes (not in a segment)", pos, (unsigned)bytes);
			return 0;
		}
		s = take_slot(bank, seg_lo(bank), seg_hi(bank) - seg_lo(bank));
		if (!s) {
			UNLOCK();
			l_perf("[music-cache] no slot for %d %s, reading from the file", bank, bank_name(bank));
			return 0;
		}
		cold = 1;
		load_range(s, bank);
		if (s->state != S_READY) { UNLOCK(); return 0; }
	}
	int bank = s->bank; uint32_t gen = s->gen;
	uint64_t t0 = now_us();
	int waited = 0;
	while (s->state == S_LOADING && s->gen == gen) {
		UNLOCK();
		sceKernelDelayThread(1000);
		waited = 1;
		if (now_us() - t0 > WAIT_MAX_US) {
			l_error("[music-cache] gave up waiting for %d %s after %llu ms", bank, bank_name(bank),
			        (unsigned long long)((now_us() - t0) / 1000));
			return 0;
		}
		if (!LOCK()) return 0;
	}
	if (s->state != S_READY || s->gen != gen ||
	    (uint64_t)pos + bytes > (uint64_t)s->off + s->len) {
		UNLOCK();
		return 0;
	}
	memcpy(ptr, s->buf + (pos - s->off), bytes);
	s->used = now_us();
	UNLOCK();
	if (bank >= 0 && !cold)
		l_perf("[music-cache] hit %d %s: %u bytes at +%ld%s", bank, bank_name(bank), (unsigned)bytes, pos,
		       waited ? " (waited for the read-ahead)" : "");
	return bytes;
}

static int add_choices(int seg, int bank, int *out, int n, int max, int depth) {
	const struct music_seg *s = &MUSIC_SEG[seg];
	for (int c = 0; c < s->n; c++) {
		int ns = MUSIC_CHOICE[s->first + c].seg;
		if (ns < 0 || ns >= MUSIC_SEG_N) continue;
		int b = MUSIC_SEG[ns].bank;
		if (b < 0 || b >= MUSIC_BANK_MAP_N) {
			if (depth < 4) n = add_choices(ns, bank, out, n, max, depth + 1);
			if (n < 0) return -1;
			continue;
		}
		if (b == bank) continue;
		int dup = 0;
		for (int i = 0; i < n; i++) if (out[i] == b) dup = 1;
		if (dup) continue;
		if (n >= max) return -1;
		out[n++] = b;
	}
	return n;
}

static int successors(int bank, int *out, int max) {
	int n = 0;
	for (int s = 0; s < MUSIC_SEG_N && n >= 0; s++)
		if (MUSIC_SEG[s].bank == bank) n = add_choices(s, bank, out, n, max, 0);
	return n;
}

static void ensure_thread(void) {
	if (g_thread >= 0) return;
	g_flag = sceKernelCreateEventFlag("music_seg_cache", 0, 0, NULL);
	if (g_flag < 0) { l_error("[music-cache] event flag: %#x", g_flag); return; }
	g_thread = sceKernelCreateThread("music_seg_cache", prefetch_thread, 0x10000100,
	                                 32 * 1024, 0, 0, NULL);
	if (g_thread < 0) { l_error("[music-cache] thread: %#x", g_thread); return; }
	sceKernelStartThread(g_thread, 0, NULL);
}

void music_seg_cache_hint(int bank) {
	if (bank < 0 || bank >= MUSIC_BANK_MAP_N) return;
	int want[WANT_MAX];
	int n = successors(bank, want, WANT_MAX);
	if (!LOCK()) return;
	ensure_thread();
	g_cur_bank = bank;
	g_hint_fopen = g_fopen_count;
	g_want_n = 0;
	g_queue_n = 0;
	int queued = 0;
	for (int i = 0; i < n; i++) {
		if (MUSIC_BANK_MAP[want[i]].length > SLOT_MAX) continue;
		g_want[g_want_n++] = want[i];
		if (!find_slot(want[i])) g_queue[g_queue_n++] = want[i];
	}
	queued = g_queue_n;
	UNLOCK();
	if (n < 0)
		l_perf("[music-cache] open %d %s: too many successors to read ahead", bank, bank_name(bank));
	else
		l_perf("[music-cache] open %d %s: %d successor(s), %d to read ahead", bank, bank_name(bank), n, queued);
	if (queued && g_flag >= 0)
		sceKernelSetEventFlag(g_flag, 1);
}

static void load_range(struct slot *s, int bank) {
	unsigned int off = s->off, cap = s->cap; uint32_t gen = s->gen;
	unsigned char *buf = s->buf;
	UNLOCK();
	uint64_t t0 = now_us();
	unsigned int got = 0;
	SceUID fd = sceIoOpen(g_path, SCE_O_RDONLY, 0);
	if (fd >= 0) {
		if (sceIoLseek(fd, off, SCE_SEEK_SET) == (SceOff)off) {
			while (got < cap) {
				unsigned int want = cap - got < READ_CHUNK ? cap - got : READ_CHUNK;
				int r = sceIoRead(fd, buf + got, want);
				if (r <= 0) break;
				got += (unsigned)r;
			}
		}
		sceIoClose(fd);
	}
	uint64_t ms = (now_us() - t0) / 1000;
	if (!LOCK()) return;
	if (s->gen != gen) return;
	s->len = got;
	s->state = got ? S_READY : S_EMPTY;
	s->used = now_us();
	if (got)
		l_perf("[music-cache] %s %d %s: %u KB in %llu ms",
		       sceKernelGetThreadId() == g_thread ? "read ahead" : "loaded on first read",
		       bank, bank_name(bank), got / 1024, (unsigned long long)ms);
	else
		l_error("[music-cache] load of %d %s failed (fd=%#x)", bank, bank_name(bank), fd);
}

static int prefetch_thread(SceSize args, void *argp) {
	(void)args; (void)argp;
	for (;;) {
		unsigned int bits = 0;
		sceKernelWaitEventFlag(g_flag, 1, SCE_EVENT_WAITOR | SCE_EVENT_WAITCLEAR, &bits, NULL);
		if (!LOCK()) continue;
		if (!g_path[0]) { UNLOCK(); continue; }
		if (g_hdr_wanted && g_hdr.state == S_EMPTY && g_hdr.cap) {
			g_hdr_wanted = 0;
			if (!g_hdr.buf) g_hdr.buf = malloc(g_hdr.cap);
			if (g_hdr.buf) { g_hdr.state = S_LOADING; g_hdr.gen++; load_range(&g_hdr, -1); }
		}
		while (g_queue_n) {
			for (int i = 0; i < 400 && (g_fopen_count == g_hint_fopen || bank_handle_open()); i++) {
				UNLOCK();
				sceKernelDelayThread(5000);
				if (!LOCK()) return 0;
				if (!g_queue_n) break;
			}
			if (!g_queue_n) break;
			int bank = g_queue[0];
			if (g_queue_n > 1)
				memmove(&g_queue[0], &g_queue[1], (size_t)(g_queue_n - 1) * sizeof(int));
			g_queue_n--;
			if (find_slot(bank)) continue;
			if (!bank_protected(bank)) continue;
			struct slot *s = take_slot(bank, seg_lo(bank), seg_hi(bank) - seg_lo(bank));
			if (!s) { l_perf("[music-cache] no slot to read ahead %d %s", bank, bank_name(bank)); continue; }
			load_range(s, bank);
		}
		UNLOCK();
	}
	return 0;
}
