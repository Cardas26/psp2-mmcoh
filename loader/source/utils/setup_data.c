#include "utils/setup_data.h"

#include <errno.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <zlib.h>

#define CHUNK (256 * 1024)
#define PATH_MAX_LEN 512

#define SIG_LOCAL 0x04034b50
#define SIG_CENTRAL 0x02014b50
#define SIG_END 0x06054b50

static const struct {
    const char *name;
    uint32_t size, crc;
} LIBS[] = {
    { "libApplication.so", 7852600, 0xb0e213d3 },
    { "libfmodex.so",       910712, 0x71d2dbe2 },
    { "libfmodevent.so",    360220, 0xe0d08194 },
};
#define LIB_DIR "lib/armeabi/"

static const char *const VIDEO_RENAMES[][2] = {
    { "Videos/C0S3_Magic Portal.mp4", "Videos/C0S3_Magic_Portal.mp4" },
    { "Videos/C0S4_Anwen Hidden.mp4", "Videos/C0S4_Anwen_Hidden.mp4" },
    { "Videos/C1S1_Stop Fight.mp4", "Videos/C1S1_Stop_Fight.mp4" },
    { "Videos/C4S4_Aidan_The_Demon.mp4", "Videos/C4S4_Aidan_the_demon.mp4" },
    { "Videos/Upsell_Video.mp4", "Videos/Upsell_video.mp4" },
    { "Videos/C2S1_Godric_Back.mp4", "Videos/C2S1_Godric_back.mp4" },
    { "Videos/C5S1_Silver_Cities.mp4", "Videos/C5S1_Silver_cities.mp4" },
};

static const char *const FIX_TARGETS[] = {
    "res/menus/layouts/advpause_SD.layout",
    "res/menus/layouts/battle_intro_SD.layout",
    "res/menus/layouts/heroSelectMenu_1vs1_SD.layout",
    "res/menus/layouts/credits.layout",
    "res/menus/looknfeel/summary.looknfeel",
    "res/menus/imagesets/mppauseonline/mppauseonline.imageset",
    "res/menus/schemes/taharezlook.scheme",
    "res/menus/schemes/artifacts_adventure.scheme",
};

static const uint8_t DEFAULT_T_PNG[] = {
    0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x48, 0x44,
    0x52, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x10, 0x08, 0x06, 0x00, 0x00, 0x00, 0x1f,
    0xf3, 0xff, 0x61, 0x00, 0x00, 0x00, 0x12, 0x49, 0x44, 0x41, 0x54, 0x78, 0xda, 0x63, 0x60,
    0x18, 0x05, 0xa3, 0x60, 0x14, 0x8c, 0x02, 0x08, 0x00, 0x00, 0x04, 0x10, 0x00, 0x01, 0xaf,
    0x45, 0x88, 0x2c, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82,
};

static int fail(setup_job *j, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(j->error, sizeof(j->error), fmt, ap);
    va_end(ap);
    return -1;
}

static int fail_write(setup_job *j, const char *path) {
    return fail(j, "Could not write %s (errno %d).\n\nCheck that the memory card "
                   "has free space, then start the game again.", path, errno);
}

static const char *base_name(const char *path) {
    const char *s = strrchr(path, '/');
    return s ? s + 1 : path;
}

static uint16_t rd16(const uint8_t *p) {
    return (uint16_t)(p[0] | p[1] << 8);
}

static uint32_t rd32(const uint8_t *p) {
    return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
}

static int starts_with(const char *s, const char *prefix) {
    return strncmp(s, prefix, strlen(prefix)) == 0;
}

static int ends_with(const char *s, const char *suffix) {
    size_t n = strlen(s), m = strlen(suffix);
    return n >= m && memcmp(s + n - m, suffix, m) == 0;
}

static int is_dir_entry(const setup_entry *e) {
    return ends_with(e->name, "/");
}

static int bad_name(const char *name) {
    for (const char *p = name; *p; p++)
        if ((*p == ' ' || *p == '.') && (p[1] == '/' || p[1] == '\0'))
            return 1;
    return 0;
}

static const char *obb_name(const char *name) {
    for (size_t i = 0; i < sizeof(VIDEO_RENAMES) / sizeof(VIDEO_RENAMES[0]); i++)
        if (strcmp(name, VIDEO_RENAMES[i][0]) == 0)
            return VIDEO_RENAMES[i][1];
    return name;
}

static int obb_extracted(const setup_entry *e) {
    return !is_dir_entry(e) && !bad_name(obb_name(e->name));
}

static const setup_entry *zip_find(const setup_zip *z, const char *name) {
    for (int i = 0; i < z->count; i++)
        if (strcmp(z->entries[i].name, name) == 0)
            return &z->entries[i];
    return NULL;
}

static int fail_incomplete(setup_job *j, const char *name, long size) {
    return fail(j, "%s is incomplete or is not a zip file (%ld bytes).\n\nCopy it to "
                   "ux0:data/mmcoh/ again. Over USB, eject the Vita in the PC's file "
                   "manager before unplugging it: the PC can report a copy as done "
                   "while it is still writing.", name, size);
}

static int fail_damaged(setup_job *j, const char *name, const char *what) {
    return fail(j, "%s is damaged: %s does not read back correctly.\n\nCopy it to "
                   "ux0:data/mmcoh/ again, then start the game again.", name, what);
}

static int zip_open(setup_job *j, setup_zip *z, const char *path) {
    const char *name = base_name(path);
    z->path = path;
    z->f = fopen(path, "rb");
    if (!z->f)
        return fail(j, "Could not open %s (errno %d).", path, errno);

    if (fseek(z->f, 0, SEEK_END) != 0)
        return fail_incomplete(j, name, -1);
    long size = ftell(z->f);
    long tail = size < 22 + 0xFFFF ? size : 22 + 0xFFFF;
    if (tail < 22)
        return fail_incomplete(j, name, size);
    uint8_t *buf = malloc(tail);
    if (!buf)
        return fail(j, "Out of memory reading %s.", name);
    if (fseek(z->f, size - tail, SEEK_SET) != 0 || fread(buf, 1, tail, z->f) != (size_t)tail) {
        free(buf);
        return fail_incomplete(j, name, size);
    }
    long eocd = -1;
    for (long i = tail - 22; i >= 0; i--) {
        if (rd32(buf + i) == SIG_END && i + 22 + rd16(buf + i + 20) == tail) {
            eocd = i;
            break;
        }
    }
    if (eocd < 0) {
        free(buf);
        return fail_incomplete(j, name, size);
    }
    int count = rd16(buf + eocd + 10);
    uint32_t cd_size = rd32(buf + eocd + 12), cd_off = rd32(buf + eocd + 16);
    long eocd_at = size - tail + eocd;
    free(buf);
    if (cd_off == 0xFFFFFFFF || (long)cd_off + (long)cd_size > eocd_at)
        return fail_damaged(j, name, "its file table");

    uint8_t *cd = malloc(cd_size);
    z->entries = calloc(count ? count : 1, sizeof(*z->entries));
    if (!cd || !z->entries) {
        free(cd);
        return fail(j, "Out of memory reading %s.", name);
    }
    if (fseek(z->f, cd_off, SEEK_SET) != 0 || fread(cd, 1, cd_size, z->f) != cd_size) {
        free(cd);
        return fail_damaged(j, name, "its file table");
    }
    uint32_t p = 0;
    for (int i = 0; i < count; i++) {
        const uint8_t *h = cd + p;
        if (p + 46 > cd_size || rd32(h) != SIG_CENTRAL)
            break;
        uint16_t flags = rd16(h + 8), nlen = rd16(h + 28);
        uint32_t next = p + 46 + nlen + rd16(h + 30) + rd16(h + 32);
        if (next > cd_size)
            break;
        setup_entry *e = &z->entries[i];
        e->method = rd16(h + 10);
        e->crc = rd32(h + 16);
        e->csize = rd32(h + 20);
        e->usize = rd32(h + 24);
        e->offset = rd32(h + 42);
        e->name = malloc(nlen + 1);
        if (!e->name)
            break;
        memcpy(e->name, h + 46, nlen);
        e->name[nlen] = '\0';
        z->count = i + 1;
        if ((flags & 1) || (e->method != 0 && e->method != 8) || e->csize == 0xFFFFFFFF
            || e->usize == 0xFFFFFFFF || e->offset == 0xFFFFFFFF) {
            free(cd);
            return fail(j, "%s is not the file the port expects: %s is stored in a way "
                           "the Android release does not use.", name, e->name);
        }
        p = next;
    }
    free(cd);
    if (z->count != count)
        return fail_damaged(j, name, "its file table");
    return 0;
}

static void zip_close(setup_zip *z) {
    if (z->f)
        fclose(z->f);
    for (int i = 0; i < z->count; i++)
        free(z->entries[i].name);
    free(z->entries);
    memset(z, 0, sizeof(*z));
}

int setup_open(setup_job *j, const char *apk, const char *obb, const char *out) {
    memset(j, 0, sizeof(*j));
    j->out = out;

    if (zip_open(j, &j->apk, apk) < 0)
        return -1;
    if (!zip_find(&j->apk, LIB_DIR "libApplication.so"))
        return fail(j, "%s is not the Clash of Heroes APK: it has no "
                       "lib/armeabi/libApplication.so.", base_name(apk));
    for (size_t i = 0; i < sizeof(LIBS) / sizeof(LIBS[0]); i++) {
        char name[64];
        snprintf(name, sizeof(name), LIB_DIR "%s", LIBS[i].name);
        const setup_entry *e = zip_find(&j->apk, name);
        if (!e || e->usize != LIBS[i].size || e->crc != LIBS[i].crc)
            return fail(j, "%s is a different version of Clash of Heroes.\n\nThe port "
                           "runs only the Android v1.4 release (versionCode 1906): its "
                           "code is patched at fixed addresses. This APK's %s is not "
                           "v1.4's.", base_name(apk), LIBS[i].name);
        j->bytes_total += e->usize;
        j->files_total++;
    }
    for (int i = 0; i < j->apk.count; i++) {
        const setup_entry *e = &j->apk.entries[i];
        if (starts_with(e->name, "assets/") && !is_dir_entry(e)) {
            j->bytes_total += e->usize;
            j->files_total++;
        }
    }

    if (zip_open(j, &j->obb, obb) < 0)
        return -1;
    for (size_t i = 0; i < sizeof(FIX_TARGETS) / sizeof(FIX_TARGETS[0]); i++)
        if (!zip_find(&j->obb, FIX_TARGETS[i]))
            return fail(j, "%s is not the Clash of Heroes v1.4 OBB: it has no %s.",
                        base_name(obb), FIX_TARGETS[i]);
    for (int i = 0; i < j->obb.count; i++) {
        const setup_entry *e = &j->obb.entries[i];
        if (obb_extracted(e)) {
            j->bytes_total += e->usize;
            j->files_total++;
        }
    }
    return 0;
}

static char made_dir[PATH_MAX_LEN];

static int make_parents(setup_job *j, const char *path) {
    const char *slash = strrchr(path, '/');
    size_t n = slash ? (size_t)(slash - path) : 0;
    if (n == 0)
        return 0;
    if (n >= sizeof(made_dir))
        return fail_write(j, path);
    if (strlen(made_dir) == n && memcmp(made_dir, path, n) == 0)
        return 0;
    char dir[PATH_MAX_LEN];
    memcpy(dir, path, n);
    dir[n] = '\0';
    for (char *p = dir + 1;; p++) {
        if (*p == '/' || *p == '\0') {
            char c = *p;
            *p = '\0';
            if (p[-1] != ':')
                mkdir(dir, 0777);
            *p = c;
            if (!c)
                break;
        }
    }
    struct stat st;
    if (stat(dir, &st) != 0 || !S_ISDIR(st.st_mode))
        return fail_write(j, dir);
    memcpy(made_dir, dir, n + 1);
    return 0;
}

static int extract(setup_job *j, setup_zip *z, const setup_entry *e, const char *dest,
                   uint8_t *in, uint8_t *out) {
    const char *zname = base_name(z->path);
    uint8_t lh[30];
    if (fseek(z->f, e->offset, SEEK_SET) != 0 || fread(lh, 1, sizeof(lh), z->f) != sizeof(lh)
        || rd32(lh) != SIG_LOCAL || fseek(z->f, rd16(lh + 26) + rd16(lh + 28), SEEK_CUR) != 0)
        return fail_damaged(j, zname, e->name);
    if (make_parents(j, dest) < 0)
        return -1;
    FILE *o = fopen(dest, "wb");
    if (!o)
        return fail_write(j, dest);

    z_stream s;
    memset(&s, 0, sizeof(s));
    if (e->method == 8 && inflateInit2(&s, -MAX_WBITS) != Z_OK) {
        fclose(o);
        return fail(j, "Out of memory unpacking %s.", zname);
    }
    uint32_t left = e->csize, produced = 0;
    uLong crc = crc32(0, NULL, 0);
    int bad = 0, write_failed = 0, done = 0;
    while (!done && !bad && !write_failed) {
        uint32_t n;
        if (e->method == 0) {
            n = left < CHUNK ? left : CHUNK;
            if (n && fread(out, 1, n, z->f) != n)
                bad = 1;
            left -= n;
            done = left == 0;
        } else {
            if (s.avail_in == 0 && left > 0) {
                uint32_t r = left < CHUNK ? left : CHUNK;
                if (fread(in, 1, r, z->f) != r) {
                    bad = 1;
                    break;
                }
                left -= r;
                s.next_in = in;
                s.avail_in = r;
            }
            s.next_out = out;
            s.avail_out = CHUNK;
            int zr = inflate(&s, Z_NO_FLUSH);
            if (zr != Z_OK && zr != Z_STREAM_END)
                bad = 1;
            done = zr == Z_STREAM_END;
            n = CHUNK - s.avail_out;
        }
        if (bad)
            break;
        crc = crc32(crc, out, n);
        produced += n;
        if (n && fwrite(out, 1, n, o) != n)
            write_failed = 1;
        j->bytes_done += n;
    }
    if (e->method == 8)
        inflateEnd(&s);
    if (fclose(o) != 0)
        write_failed = 1;
    if (write_failed)
        return fail_write(j, dest);
    if (bad || produced != e->usize || crc != e->crc)
        return fail_damaged(j, zname, e->name);
    j->files_done++;
    return 0;
}

typedef struct {
    char *p;
    size_t n;
} buf;

static void dlc_path(setup_job *j, const char *rel, char *path) {
    snprintf(path, PATH_MAX_LEN, "%sDLC/%s", j->out, rel);
}

static int exists(setup_job *j, const char *rel) {
    char path[PATH_MAX_LEN];
    dlc_path(j, rel, path);
    struct stat st;
    return stat(path, &st) == 0;
}

static int load(setup_job *j, const char *rel, buf *b) {
    char path[PATH_MAX_LEN];
    dlc_path(j, rel, path);
    FILE *f = fopen(path, "rb");
    if (!f)
        return 0;
    long n = -1;
    if (fseek(f, 0, SEEK_END) == 0)
        n = ftell(f);
    b->p = n >= 0 ? malloc(n + 1) : NULL;
    if (!b->p || fseek(f, 0, SEEK_SET) != 0 || fread(b->p, 1, n, f) != (size_t)n) {
        fclose(f);
        free(b->p);
        b->p = NULL;
        return fail(j, "Could not read back %s (errno %d).", path, errno);
    }
    fclose(f);
    b->p[n] = '\0';
    b->n = n;
    return 1;
}

static int load_required(setup_job *j, const char *rel, buf *b) {
    int r = load(j, rel, b);
    if (r == 0)
        return fail(j, "%s is missing after unpacking.", rel);
    return r;
}

static int save(setup_job *j, const char *rel, const void *p, size_t n) {
    char path[PATH_MAX_LEN];
    dlc_path(j, rel, path);
    FILE *f = fopen(path, "wb");
    if (!f)
        return fail_write(j, path);
    int ok = fwrite(p, 1, n, f) == n;
    if (fclose(f) != 0)
        ok = 0;
    return ok ? 0 : fail_write(j, path);
}

static const char *memfind(const char *hay, size_t hn, const char *needle) {
    size_t nn = strlen(needle);
    if (nn == 0 || hn < nn)
        return NULL;
    for (size_t i = 0; i + nn <= hn; i++)
        if (hay[i] == needle[0] && memcmp(hay + i, needle, nn) == 0)
            return hay + i;
    return NULL;
}

static const char *memfind_last(const char *hay, size_t hn, const char *needle) {
    size_t nn = strlen(needle);
    if (nn == 0 || hn < nn)
        return NULL;
    for (size_t i = hn - nn + 1; i-- > 0;)
        if (memcmp(hay + i, needle, nn) == 0)
            return hay + i;
    return NULL;
}

static int has(const buf *b, const char *needle) {
    return memfind(b->p, b->n, needle) != NULL;
}

static void to_lf(buf *b) {
    size_t w = 0;
    for (size_t r = 0; r < b->n; r++) {
        char c = b->p[r];
        if (c == '\r') {
            if (r + 1 < b->n && b->p[r + 1] == '\n')
                continue;
            c = '\n';
        }
        b->p[w++] = c;
    }
    b->n = w;
    b->p[w] = '\0';
}

static int load_text(setup_job *j, const char *rel, buf *b) {
    int r = load_required(j, rel, b);
    if (r > 0)
        to_lf(b);
    return r;
}

static int splice(setup_job *j, buf *b, size_t at, size_t cut, const char *text) {
    size_t tn = strlen(text);
    char *p = malloc(b->n - cut + tn + 1);
    if (!p)
        return fail(j, "Out of memory applying the patches.");
    memcpy(p, b->p, at);
    memcpy(p + at, text, tn);
    memcpy(p + at + tn, b->p + at + cut, b->n - at - cut);
    b->n = b->n - cut + tn;
    p[b->n] = '\0';
    free(b->p);
    b->p = p;
    return 0;
}

static int insert_after(setup_job *j, buf *b, const char *anchor, const char *line) {
    const char *a = memfind(b->p, b->n, anchor);
    if (!a)
        return 0;
    char text[512];
    snprintf(text, sizeof(text), "\n%s", line);
    if (splice(j, b, (a - b->p) + strlen(anchor), 0, text) < 0)
        return -1;
    return 1;
}

static int replace_all(setup_job *j, buf *b, const char *old, const char *new_) {
    int count = 0;
    size_t from = 0;
    const char *hit;
    while ((hit = memfind(b->p + from, b->n - from, old)) != NULL) {
        size_t at = hit - b->p;
        if (splice(j, b, at, strlen(old), new_) < 0)
            return -1;
        from = at + strlen(new_);
        count++;
    }
    return count;
}

typedef int (*line_pred)(const char *line, size_t n, void *ctx);

static size_t filter_lines(buf *b, line_pred pred, void *ctx, int keep_ends) {
    size_t w = 0, dropped = 0, r = 0;
    int first = 1;
    for (;;) {
        const char *nl = memchr(b->p + r, '\n', b->n - r);
        size_t end = nl ? (size_t)(nl - b->p) : b->n;
        if (pred(b->p + r, end - r, ctx)) {
            dropped++;
        } else {
            if (!keep_ends && !first)
                b->p[w++] = '\n';
            memmove(b->p + w, b->p + r, end - r);
            w += end - r;
            if (keep_ends && nl)
                b->p[w++] = '\n';
            first = 0;
        }
        if (!nl)
            break;
        r = end + 1;
    }
    if (dropped) {
        b->n = w;
        b->p[w] = '\0';
    }
    return dropped;
}

static int pred_literal(const char *line, size_t n, void *ctx) {
    const char *const *lits = ctx;
    for (; *lits; lits++)
        if (memfind(line, n, *lits))
            return 1;
    return 0;
}

static int pred_summary_fontscale(const char *line, size_t n, void *ctx) {
    (void)ctx;
    static const char head[] = "<Property name=\"FontScale\" value=\"";
    size_t from = 0;
    const char *hit;
    while ((hit = memfind(line + from, n - from, head)) != NULL) {
        size_t i = (hit - line) + sizeof(head) - 1;
        while (i < n && ((line[i] >= '0' && line[i] <= '9') || line[i] == '.'))
            i++;
        if (n - i >= 4 && memcmp(line + i, "\" />", 4) == 0)
            return 1;
        from = (hit - line) + 1;
    }
    return 0;
}

static int sed_delete(setup_job *j, const char *rel, line_pred pred, void *ctx) {
    buf b;
    if (load_required(j, rel, &b) < 0)
        return -1;
    int rc = filter_lines(&b, pred, ctx, 0) ? save(j, rel, b.p, b.n) : 0;
    free(b.p);
    return rc;
}

static int fix_1(setup_job *j) {
    static const char *const names[] = {
        "advpause.layout", "advpause_SD.layout", "advpause_iOS.layout",
        "setting_option.layout", "setting_option_SD.layout", "mppauseonline.layout",
        "mppauseonline_SD.layout", "titlemenu.layout", "titlemenu_SD.layout",
        "titlemenu_Amazon.layout", "titlemenu_iOS.layout", "help_option.layout",
        "help_option_SD.layout",
    };
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        char rel[128];
        snprintf(rel, sizeof(rel), "res/menus/layouts/%s", names[i]);
        buf b;
        int r = load(j, rel, &b);
        if (r < 0)
            return -1;
        if (r == 0)
            continue;
        int count = replace_all(j, &b, "Value=\"DejaVuSans-10\"", "Value=\"basefont\"");
        int rc = count > 0 ? save(j, rel, b.p, b.n) : count;
        free(b.p);
        if (rc < 0)
            return -1;
    }
    return 0;
}

static int fix_2_3(setup_job *j) {
    static const char rel[] = "res/menus/layouts/advpause_SD.layout";
    buf b;
    if (load_text(j, rel, &b) < 0)
        return -1;
    int changed = 0, rc = 0;
    if (!has(&b, "Name=\"advpause/infos\"")) {
        static const char tail[] = "    </Window>\n</GUILayout>";
        size_t e = b.n, tn = sizeof(tail) - 1;
        while (e > 0 && strchr(" \t\n\r\f\v", b.p[e - 1]))
            e--;
        if (e < tn || memcmp(b.p + e - tn, tail, tn) != 0) {
            free(b.p);
            return fail(j, "Data fix 2: %s does not end as expected.", rel);
        }
        rc = splice(j, &b, e - tn, 0,
            "        <Window Type=\"taharezlook/LineText\" Name=\"advpause/infos\" >\n"
            "            <Property Name=\"FrameEnabled\" Value=\"False\" />\n"
            "            <Property Name=\"HorzFormatting\" Value=\"RightAligned\" />\n"
            "            <Property Name=\"UnifiedAreaRect\" Value=\"{{0.912,0},{0.95,0},{0.914,0},{0.954,0}}\" />\n"
            "            <Property Name=\"BackgroundEnabled\" Value=\"False\" />\n"
            "        </Window>\n");
        changed = 1;
    }
    if (rc == 0 && !has(&b, "Name=\"advpause/save__auto__text\"")) {
        static const char open[] = "        <Window Type=\"lbut\" Name=\"advpause/save\" >\n";
        static const char close[] = "        </Window>\n";
        const char *o = memfind(b.p, b.n, open);
        size_t at = 0;
        if (o) {
            for (size_t r = (o - b.p) + sizeof(open) - 1; r < b.n;) {
                const char *nl = memchr(b.p + r, '\n', b.n - r);
                if (!nl)
                    break;
                if ((size_t)(nl - b.p) + 1 - r == sizeof(close) - 1
                    && memcmp(b.p + r, close, sizeof(close) - 1) == 0) {
                    at = r;
                    break;
                }
                r = (nl - b.p) + 1;
            }
        }
        if (!at) {
            free(b.p);
            return fail(j, "Data fix 3: advpause/save not found in %s.", rel);
        }
        rc = splice(j, &b, at, 0,
            "            <Window Type=\"taharezlook/LineText\" Name=\"advpause/save__auto__text\" >\n"
            "                <Property Name=\"FrameEnabled\" Value=\"False\" />\n"
            "                <Property Name=\"BackgroundEnabled\" Value=\"False\" />\n"
            "                <Property Name=\"Font\" Value=\"basefont\" />\n"
            "                <Property Name=\"UnifiedAreaRect\" Value=\"{{0,0},{0,0},{0.01,0},{0.01,0}}\" />\n"
            "            </Window>\n");
        changed = 1;
    }
    if (rc == 0 && changed)
        rc = save(j, rel, b.p, b.n);
    free(b.p);
    return rc;
}

static int insert_fix(setup_job *j, const char *rel, const char *unless, const char *anchor,
                      const char *line) {
    buf b;
    if (load_required(j, rel, &b) < 0)
        return -1;
    int rc = 0;
    if (!has(&b, unless)) {
        rc = insert_after(j, &b, anchor, line);
        if (rc > 0)
            rc = save(j, rel, b.p, b.n);
    }
    free(b.p);
    return rc;
}

static int fix_4(setup_job *j) {
    return insert_fix(j, "res/menus/imagesets/mppauseonline/mppauseonline.imageset",
                      "Name=\"stat/infos_backg\"",
                      "<Image Height=\"606\" Name=\"stats_frame\" Width=\"914\" XPos=\"2\" YPos=\"2\"/>",
                      "\t<Image Height=\"1\" Name=\"stat/infos_backg\" Width=\"1\" XPos=\"2\" YPos=\"2\"/>");
}

static int fix_5_6_9(setup_job *j) {
    static const char *const fix5[] = { "<Property Name=\"FontScale\" Value=\"2\" />", NULL };
    static const char *const fix9[] = { "<Property Name=\"FontScale\" Value=\"0.8\" />",
                                        "<Property Name=\"FontScale\" Value=\"0.68\" />", NULL };
    if (sed_delete(j, "res/menus/layouts/battle_intro_SD.layout", pred_literal, (void *)fix5) < 0
        || sed_delete(j, "res/menus/looknfeel/summary.looknfeel", pred_summary_fontscale, NULL) < 0
        || sed_delete(j, "res/menus/layouts/heroSelectMenu_1vs1_SD.layout", pred_literal, (void *)fix9) < 0)
        return -1;
    return 0;
}

static int fix_7(setup_job *j) {
    static const char font_rel[] = "res/menus/fonts/DejaVuSans-10.font";
    static const char font[] =
        "<Font Name=\"DejaVuSans-10\" Filename=\"basefont.imageset\" Type=\"Pixmap\" "
        "NativeHorzRes=\"1920\" NativeVertRes=\"1080\" AutoScaled=\"true\" />\n";
    if (!exists(j, font_rel) && save(j, font_rel, font, sizeof(font) - 1) < 0)
        return -1;
    return insert_fix(j, "res/menus/schemes/taharezlook.scheme", "Name=\"DejaVuSans-10\"",
                      "<Font Name=\"basefont\" Filename=\"basefont.font\" />",
                      "\t<Font Name=\"DejaVuSans-10\" Filename=\"DejaVuSans-10.font\" />");
}

static int fix_8(setup_job *j) {
    return insert_fix(j, "res/menus/schemes/artifacts_adventure.scheme", "Name=\"widget_artifacts\"",
                      "<Imageset Name=\"widget_artifacts_adventure\" Filename=\"widget_artifacts_adventure.imageset\" />",
                      "    <Imageset Name=\"widget_artifacts\" Filename=\"widget_artifacts.imageset\" />");
}

typedef struct {
    char **items;
    int count, cap;
} strlist;

static int strlist_add(strlist *l, const char *s, size_t n) {
    if (l->count == l->cap) {
        int cap = l->cap ? l->cap * 2 : 64;
        char **items = realloc(l->items, cap * sizeof(*items));
        if (!items)
            return -1;
        l->items = items;
        l->cap = cap;
    }
    char *c = malloc(n + 1);
    if (!c)
        return -1;
    memcpy(c, s, n);
    c[n] = '\0';
    l->items[l->count++] = c;
    return 0;
}

static int strlist_find(const strlist *l, const char *s, size_t n) {
    for (int i = 0; i < l->count; i++)
        if (strlen(l->items[i]) == n && memcmp(l->items[i], s, n) == 0)
            return i;
    return -1;
}

static void strlist_free(strlist *l) {
    for (int i = 0; i < l->count; i++)
        free(l->items[i]);
    free(l->items);
    memset(l, 0, sizeof(*l));
}

static int cmp_str(const void *a, const void *b) {
    return strcmp(*(char *const *)a, *(char *const *)b);
}

static int list_dir(setup_job *j, const char *dir, const char *ext, strlist *out) {
    size_t dn = strlen(dir);
    for (int i = 0; i < j->obb.count; i++) {
        const setup_entry *e = &j->obb.entries[i];
        if (!obb_extracted(e) || strncmp(e->name, dir, dn) != 0)
            continue;
        const char *leaf = e->name + dn;
        if (strchr(leaf, '/') || !ends_with(leaf, ext))
            continue;
        if (strlist_add(out, e->name, strlen(e->name)) < 0)
            return fail(j, "Out of memory applying the patches.");
    }
    qsort(out->items, out->count, sizeof(*out->items), cmp_str);
    return 0;
}

static int capture(const char *line, size_t n, const char *prefix, int ws_eq,
                   const char **val, size_t *vn) {
    size_t pn = strlen(prefix), from = 0;
    const char *hit;
    while ((hit = memfind(line + from, n - from, prefix)) != NULL) {
        size_t i = (hit - line) + pn;
        from = (hit - line) + 1;
        if (ws_eq) {
            while (i < n && strchr(" \t\r\f\v", line[i]) && line[i])
                i++;
            if (n - i < 2 || line[i] != '=' || line[i + 1] != '"')
                continue;
            i += 2;
        }
        const char *q = memchr(line + i, '"', n - i);
        if (!q)
            continue;
        *val = line + i;
        *vn = q - (line + i);
        return 1;
    }
    return 0;
}

static int defines_fontscale(const char *line, size_t n) {
    static const char tag[] = "<PropertyDefinition";
    size_t from = 0;
    const char *hit;
    while ((hit = memfind(line + from, n - from, tag)) != NULL) {
        size_t s = (hit - line) + sizeof(tag) - 1;
        const char *gt = memchr(line + s, '>', n - s);
        size_t e = gt ? (size_t)(gt - line) : n;
        const char *a = memfind(line + s, e - s, "ame=\"FontScale\"");
        while (a) {
            if (a > line + s && (a[-1] == 'N' || a[-1] == 'n'))
                return 1;
            a = memfind(a + 1, e - (a + 1 - line), "ame=\"FontScale\"");
        }
        from = (hit - line) + 1;
    }
    return 0;
}

typedef struct {
    const strlist *valid, *types, *looks;
    const char *type_prefix;
    int type_ws_eq, layout;
    char cur[256];
    size_t cur_n;
    int have_cur;
} sweep_ctx;

static int ordered(const char *line, size_t n, const char *a, const char *b) {
    const char *x = memfind(line, n, a);
    const char *y = memfind_last(line, n, b);
    return x && y && y >= x + strlen(a);
}

static int pred_sweep(const char *line, size_t n, void *vctx) {
    sweep_ctx *c = vctx;
    const char *v;
    size_t vn;
    if (capture(line, n, c->type_prefix, c->type_ws_eq, &v, &vn)) {
        c->cur_n = vn < sizeof(c->cur) ? vn : 0;
        memcpy(c->cur, v, c->cur_n);
        c->have_cur = vn < sizeof(c->cur);
    }
    int prop = c->layout
        ? ordered(line, n, "FontScale", "Property") || ordered(line, n, "Property", "FontScale")
        : memfind(line, n, "<Property name=\"FontScale\" value=") != NULL;
    if (!prop)
        return 0;
    if (!c->have_cur)
        return 1;
    const char *look = c->cur;
    size_t ln = c->cur_n;
    int t = strlist_find(c->types, c->cur, c->cur_n);
    if (t >= 0) {
        look = c->looks->items[t];
        ln = strlen(look);
    }
    return strlist_find(c->valid, look, ln) < 0;
}

static int sweep(setup_job *j, sweep_ctx *c, const char *dir, const char *ext) {
    strlist files = { 0 };
    int rc = list_dir(j, dir, ext, &files);
    for (int i = 0; rc == 0 && i < files.count; i++) {
        const char *rel = files.items[i];
        buf b;
        if (load_text(j, rel, &b) < 0) {
            rc = -1;
            break;
        }
        c->have_cur = 0;
        if (filter_lines(&b, pred_sweep, c, 1))
            rc = save(j, rel, b.p, b.n);
        free(b.p);
    }
    strlist_free(&files);
    return rc;
}

static int fix_10(setup_job *j) {
    strlist lnf = { 0 }, schemes = { 0 }, valid = { 0 }, types = { 0 }, looks = { 0 };
    int rc = list_dir(j, "res/menus/looknfeel/", ".looknfeel", &lnf);
    if (rc == 0)
        rc = list_dir(j, "res/menus/schemes/", ".scheme", &schemes);

    for (int i = 0; rc == 0 && i < lnf.count; i++) {
        buf b;
        if (load_text(j, lnf.items[i], &b) < 0) {
            rc = -1;
            break;
        }
        const char *cur = NULL;
        size_t cur_n = 0;
        for (size_t r = 0; r < b.n && rc == 0;) {
            const char *nl = memchr(b.p + r, '\n', b.n - r);
            size_t end = nl ? (size_t)(nl - b.p) + 1 : b.n;
            const char *v;
            size_t vn;
            if (capture(b.p + r, end - r, "<WidgetLook name=\"", 0, &v, &vn)) {
                cur = v;
                cur_n = vn;
            }
            if (cur && cur_n && defines_fontscale(b.p + r, end - r)
                && strlist_find(&valid, cur, cur_n) < 0 && strlist_add(&valid, cur, cur_n) < 0)
                rc = fail(j, "Out of memory applying the patches.");
            r = end;
        }
        free(b.p);
    }

    for (int i = 0; rc == 0 && i < schemes.count; i++) {
        buf b;
        if (load_text(j, schemes.items[i], &b) < 0) {
            rc = -1;
            break;
        }
        size_t from = 0;
        const char *tag;
        while (rc == 0 && (tag = memfind(b.p + from, b.n - from, "<FalagardMapping")) != NULL) {
            from = (tag - b.p) + 1;
            const char *gt = memchr(tag, '>', b.n - (tag - b.p));
            size_t seg = gt ? (size_t)(gt - tag) : b.n - (tag - b.p);
            const char *ln = memfind_last(tag, seg, "LookNFeel=\"");
            if (!ln)
                continue;
            const char *wt = memfind_last(tag, ln - tag, "WindowType=\"");
            if (!wt)
                continue;
            const char *wv = wt + 12, *lv = ln + 11;
            const char *wq = memchr(wv, '"', ln - wv);
            const char *lq = memchr(lv, '"', b.n - (lv - b.p));
            if (!wq || !lq)
                continue;
            int t = strlist_find(&types, wv, wq - wv);
            if (t >= 0) {
                free(looks.items[t]);
                looks.items[t] = NULL;
                char *c = malloc(lq - lv + 1);
                if (!c) {
                    rc = fail(j, "Out of memory applying the patches.");
                    break;
                }
                memcpy(c, lv, lq - lv);
                c[lq - lv] = '\0';
                looks.items[t] = c;
            } else if (strlist_add(&types, wv, wq - wv) < 0 || strlist_add(&looks, lv, lq - lv) < 0) {
                rc = fail(j, "Out of memory applying the patches.");
            }
            from = (lq - b.p) + 1;
        }
        free(b.p);
    }

    if (rc == 0) {
        sweep_ctx c = { &valid, &types, &looks, "<Window Type=\"", 0, 1, "", 0, 0 };
        rc = sweep(j, &c, "res/menus/layouts/", ".layout");
    }
    if (rc == 0) {
        sweep_ctx c = { &valid, &types, &looks, "<Child type", 1, 0, "", 0, 0 };
        rc = sweep(j, &c, "res/menus/looknfeel/", ".looknfeel");
    }
    strlist_free(&lnf);
    strlist_free(&schemes);
    strlist_free(&valid);
    strlist_free(&types);
    strlist_free(&looks);
    return rc;
}

static int fix_12(setup_job *j) {
    static const char *const lit[] = { "<Property Name=\"HorzFlip\" Value=\"\" />", NULL };
    return sed_delete(j, "res/menus/layouts/credits.layout", pred_literal, (void *)lit);
}

static int fix_13(setup_job *j) {
    if (exists(j, "defaultT.png"))
        return 0;
    return save(j, "defaultT.png", DEFAULT_T_PNG, sizeof(DEFAULT_T_PNG));
}

int setup_extract(setup_job *j) {
    uint8_t *in = malloc(CHUNK), *out = malloc(CHUNK);
    char dest[PATH_MAX_LEN];
    int rc = 0;
    made_dir[0] = '\0';
    if (!in || !out) {
        free(in);
        free(out);
        return fail(j, "Out of memory starting the setup.");
    }

    j->stage = SETUP_STAGE_APK;
    for (size_t i = 0; rc == 0 && i < sizeof(LIBS) / sizeof(LIBS[0]); i++) {
        char name[64];
        snprintf(name, sizeof(name), LIB_DIR "%s", LIBS[i].name);
        snprintf(dest, sizeof(dest), "%s%s", j->out, LIBS[i].name);
        rc = extract(j, &j->apk, zip_find(&j->apk, name), dest, in, out);
    }
    for (int i = 0; rc == 0 && i < j->apk.count; i++) {
        const setup_entry *e = &j->apk.entries[i];
        if (!starts_with(e->name, "assets/") || is_dir_entry(e))
            continue;
        snprintf(dest, sizeof(dest), "%s%s", j->out, e->name);
        rc = extract(j, &j->apk, e, dest, in, out);
    }

    j->stage = SETUP_STAGE_OBB;
    for (int i = 0; rc == 0 && i < j->obb.count; i++) {
        const setup_entry *e = &j->obb.entries[i];
        if (!obb_extracted(e))
            continue;
        snprintf(dest, sizeof(dest), "%sDLC/%s", j->out, obb_name(e->name));
        rc = extract(j, &j->obb, e, dest, in, out);
    }
    free(in);
    free(out);
    if (rc < 0)
        return -1;

    j->stage = SETUP_STAGE_FIXES;
    if (fix_1(j) < 0 || fix_2_3(j) < 0 || fix_4(j) < 0 || fix_5_6_9(j) < 0 || fix_7(j) < 0
        || fix_8(j) < 0 || fix_10(j) < 0 || fix_12(j) < 0 || fix_13(j) < 0)
        return -1;
    j->stage = SETUP_STAGE_DONE;
    return 0;
}

void setup_close(setup_job *j) {
    zip_close(&j->apk);
    zip_close(&j->obb);
}
