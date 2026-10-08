#include "utils/campaign_tap.h"
#include "utils/logger.h"
#include "utils/utils.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <zlib.h>
#include <psp2/io/fcntl.h>
#include <psp2/kernel/processmgr.h>

int campaign_select_unlocked_ordinals(void);

#define MOIMAGE_PATH DATA_PATH "DLC/res/menus/imagesets/campaign_selection/campaign_selection.moimage"

#define MOIMAGE_MAGIC 0x1e240u
#define TEX_W 1244
#define TEX_H 1436
#define ALPHA_SOLID 8
#define ALPHA_VISIBLE 1

#define CELL 4
#define GRID_W (SCREEN_NATIVE_W / CELL)
#define GRID_H (SCREEN_NATIVE_H / CELL)
#define FAINT_SHIFT 5
#define TITLE_SHIFT 10

typedef struct {
	float x0, y0, x1, y1;
	uint16_t u, v, w, h;
} piece_t;

typedef struct {
	const char *name;
	int ordinal;
	piece_t art, title;
} campaign_t;

#define N_CAMPAIGNS 5
static const campaign_t k_campaigns[N_CAMPAIGNS] = {
	{ "Sylvan",     1, { 0.0973958f, 0.0f,      0.410417f, 0.615741f,  857,  797, 301, 333 },
	                   { 0.1875f,    0.344444f, 0.410417f, 0.456481f,    2,  645, 428, 148 } },
	{ "Haven",      2, { 0.35625f,   0.0f,      0.645312f, 0.653704f,    2,  797, 278, 353 },
	                   { 0.352083f,  0.285185f, 0.644271f, 0.444444f,  585,  294, 561, 170 } },
	{ "Inferno",    4, { 0.608854f,  0.0f,      0.90625f,  0.512037f,    2, 1154, 286, 277 },
	                   { 0.617708f,  0.335185f, 0.834375f, 0.440741f,  434,  645, 416, 133 } },
	{ "Necropolis", 3, { 0.576563f,  0.47037f,  0.741667f, 0.946296f, 1072,  468, 159, 256 },
	                   { 0.550521f,  0.671296f, 0.804688f, 0.794444f,  580,  468, 488, 133 } },
	{ "Academy",    5, { 0.0921875f, 0.510185f, 0.690104f, 1.0f,         2,  376, 574, 265 },
	                   { 0.210417f,  0.72963f,  0.522917f, 0.832407f,  585,  159, 600, 131 } },
};

#define N_PIECES (2 * N_CAMPAIGNS)

static uint16_t g_mask[GRID_H][GRID_W];
static int g_mask_state;

static int16_t g_vrow[N_PIECES][GRID_H];
static int16_t g_ucol[N_PIECES][GRID_W];

static const piece_t *piece(int k) {
	const campaign_t *c = &k_campaigns[k % N_CAMPAIGNS];
	return k < N_CAMPAIGNS ? &c->art : &c->title;
}

static uint16_t piece_bit(int k, int a) {
	if (k >= N_CAMPAIGNS) {
		return a >= ALPHA_SOLID ? (uint16_t)(1u << (TITLE_SHIFT + k - N_CAMPAIGNS)) : 0;
	}
	return a >= ALPHA_SOLID   ? (uint16_t)(1u << k) :
	       a >= ALPHA_VISIBLE ? (uint16_t)(1u << (FAINT_SHIFT + k)) : 0;
}

static int16_t sample(float s, float s0, float s1, int t0, int tn) {
	if (s < s0 || s >= s1) {
		return -1;
	}
	int t = (int)((s - s0) / (s1 - s0) * (float)tn);
	return (int16_t)(t0 + (t < tn ? t : tn - 1));
}

static void plan_samples(void) {
	for (int k = 0; k < N_PIECES; k++) {
		const piece_t *p = piece(k);
		for (int gy = 0; gy < GRID_H; gy++) {
			g_vrow[k][gy] = sample((float)(gy * CELL + CELL / 2),
				p->y0 * SCREEN_NATIVE_H, p->y1 * SCREEN_NATIVE_H, p->v, p->h);
		}
		for (int gx = 0; gx < GRID_W; gx++) {
			g_ucol[k][gx] = sample((float)(gx * CELL + CELL / 2),
				p->x0 * SCREEN_NATIVE_W, p->x1 * SCREEN_NATIVE_W, p->u, p->w);
		}
	}
}

static void mask_row(int v, const uint8_t *row, int *next_gy) {
	for (int k = 0; k < N_PIECES; k++) {
		while (next_gy[k] < GRID_H && g_vrow[k][next_gy[k]] == v) {
			int gy = next_gy[k]++;
			for (int gx = 0; gx < GRID_W; gx++) {
				int u = g_ucol[k][gx];
				if (u >= 0) {
					g_mask[gy][gx] |= piece_bit(k, row[u * 2] & 0x0f);
				}
			}
		}
	}
}

static int read_mask(void) {
	SceUID fd = sceIoOpen(MOIMAGE_PATH, SCE_O_RDONLY, 0);
	if (fd < 0) {
		l_warn("[campaign-tap] cannot open %s (0x%08x); the campaign select "
			"takes a pick on the titles only", MOIMAGE_PATH, fd);
		return -1;
	}
	uint32_t hdr[10];
	if (sceIoRead(fd, hdr, sizeof(hdr)) != (int)sizeof(hdr) ||
	    hdr[0] != MOIMAGE_MAGIC || hdr[2] != TEX_W || hdr[3] != TEX_H ||
	    hdr[4] != 4 || hdr[5] != 1 || hdr[8] != (uint32_t)(TEX_W * TEX_H * 2)) {
		l_warn("[campaign-tap] %s is not the 1244x1436 RGBA4444 image this "
			"was written against; the campaign select takes a pick on the "
			"titles only", MOIMAGE_PATH);
		sceIoClose(fd);
		return -1;
	}

	static uint8_t in[16384];
	static uint8_t row[TEX_W * 2];
	z_stream zs;
	memset(&zs, 0, sizeof(zs));
	if (inflateInit(&zs) != Z_OK) {
		sceIoClose(fd);
		return -1;
	}

	plan_samples();
	memset(g_mask, 0, sizeof(g_mask));
	int next_gy[N_PIECES];
	for (int k = 0; k < N_PIECES; k++) {
		next_gy[k] = 0;
		while (next_gy[k] < GRID_H && g_vrow[k][next_gy[k]] < 0) {
			next_gy[k]++;
		}
	}
	int ok = 1;
	for (int v = 0; v < TEX_H && ok; v++) {
		zs.next_out = row;
		zs.avail_out = sizeof(row);
		while (zs.avail_out) {
			if (!zs.avail_in) {
				int n = sceIoRead(fd, in, sizeof(in));
				if (n <= 0) {
					ok = 0;
					break;
				}
				zs.next_in = in;
				zs.avail_in = (uInt)n;
			}
			int r = inflate(&zs, Z_NO_FLUSH);
			if (r == Z_STREAM_END ? zs.avail_out != 0 : r != Z_OK) {
				ok = 0;
				break;
			}
		}
		if (ok) {
			mask_row(v, row, next_gy);
		}
	}
	inflateEnd(&zs);
	sceIoClose(fd);
	if (!ok) {
		l_warn("[campaign-tap] %s did not inflate; the campaign select takes "
			"a pick on the titles only", MOIMAGE_PATH);
		return -1;
	}
	return 0;
}

int campaign_tap_pin(float x, float y, float *px, float *py) {
	int enabled = campaign_select_unlocked_ordinals();
	if (enabled < 0) {
		return 0;
	}
	if (g_mask_state == 0) {
		SceUInt64 t0 = sceKernelGetProcessTimeWide();
		g_mask_state = read_mask() == 0 ? 1 : -1;
		if (g_mask_state > 0) {
			l_info("[campaign-tap] picture shapes read in %u ms",
				(unsigned)((sceKernelGetProcessTimeWide() - t0) / 1000));
		}
	}
	if (g_mask_state < 0) {
		return 0;
	}

	int gx = (int)x / CELL, gy = (int)y / CELL;
	if (gx < 0 || gx >= GRID_W || gy < 0 || gy >= GRID_H) {
		return 0;
	}
	uint16_t m = g_mask[gy][gx];
	int pick = -1;
	uint16_t titles = m >> TITLE_SHIFT;
	if (titles) {
		for (int i = N_CAMPAIGNS - 1; i >= 0 && pick < 0; i--) {
			if (titles & (1u << i)) {
				pick = i;
			}
		}
		if (!(enabled & (1 << k_campaigns[pick].ordinal))) {
			return 0;
		}
	} else {
		for (int shift = 0; shift <= FAINT_SHIFT && pick < 0; shift += FAINT_SHIFT) {
			for (int i = N_CAMPAIGNS - 1; i >= 0 && pick < 0; i--) {
				if ((m & (1u << (shift + i))) &&
				    (enabled & (1 << k_campaigns[i].ordinal))) {
					pick = i;
				}
			}
		}
		if (pick < 0) {
			return 0;
		}
	}

	const piece_t *t = &k_campaigns[pick].title;
	*px = (t->x0 + t->x1) * 0.5f * SCREEN_NATIVE_W;
	*py = (t->y0 + t->y1) * 0.5f * SCREEN_NATIVE_H;
	l_debug("[campaign-tap] (%d,%d) is %s's %s -> its title at (%d,%d)",
		(int)x, (int)y, k_campaigns[pick].name,
		titles ? "title" : (m & (1u << pick)) ? "picture" : "faint edge",
		(int)*px, (int)*py);
	return 1;
}
