#include "subtitles.h"

#include "utils/logger.h"
#include "utils/utils.h"

#include <psp2/pgf.h>
#include <psp2/sysmodule.h>
#include <vitaGL.h>

#include <malloc.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define SCREEN_W ((float)SCREEN_NATIVE_W)
#define SCREEN_H ((float)SCREEN_NATIVE_H)

#define SP_TO_PX 1.5f

#define CAP_HEIGHT_EM 0.716f

#define SHADOW_PX 1.f

#define ATLAS_W 512
#define ATLAS_H 512
#define ATLAS_PAD 1
#define MAX_GLYPHS 512
#define MAX_LIVE 8
#define MAX_CHARS 512
#define MAX_LINES 16

typedef struct {
    uint32_t cp;
    int w, h, left, top;
    float adv;
    float u0, v0, u1, v1;
} glyph_t;

typedef struct {
    long long id;
    int nquads;
    float *pos, *uv;
    float col[4];
} sub_t;

static glyph_t g_glyphs[MAX_GLYPHS];
static int g_nglyphs;
static int g_shelf_x, g_shelf_y, g_shelf_h;
static bool g_atlas_full_warned;

static sub_t g_live[MAX_LIVE];
static long long g_next_id = 1;

static SceFontLibHandle g_lib;
static SceFontHandle g_font;
static bool g_font_failed;
static float g_cap_px, g_ascent_px, g_line_px;

static GLuint g_program, g_atlas;
static GLint g_u_tex, g_u_col, g_u_off;
static bool g_gl_ready, g_gl_failed;

static void *pgf_alloc(void *ud, unsigned int size) { (void)ud; return malloc(size); }
static void pgf_free(void *ud, void *p) { (void)ud; free(p); }

static bool font_setup(unsigned px) {
    if (g_font)
        return true;
    if (g_font_failed)
        return false;
    g_font_failed = true;

    int ret = sceSysmoduleLoadModule(SCE_SYSMODULE_PGF);
    if (ret < 0) {
        l_error("[subtitles] sceSysmoduleLoadModule(PGF) failed: %#x", ret);
        return false;
    }
    SceFontNewLibParams params;
    memset(&params, 0, sizeof(params));
    params.numFonts = 1;
    params.allocFunc = pgf_alloc;
    params.freeFunc = pgf_free;
    unsigned err = 0;
    g_lib = sceFontNewLib(&params, &err);
    if (err || !g_lib) {
        l_error("[subtitles] sceFontNewLib failed: %#x", err);
        return false;
    }
    SceFontStyle style;
    memset(&style, 0, sizeof(style));
    style.fontH = style.fontV = (float)px;
    style.fontLanguage = SCE_FONT_LANGUAGE_LATIN;
    int index = sceFontFindOptimumFont(g_lib, &style, &err);
    if (err) {
        l_error("[subtitles] sceFontFindOptimumFont failed: %#x", err);
        return false;
    }
    g_font = sceFontOpen(g_lib, index, 0, &err);
    if (err || !g_font) {
        l_error("[subtitles] sceFontOpen(%d) failed: %#x", index, err);
        g_font = NULL;
        return false;
    }
    SceFontInfo info;
    memset(&info, 0, sizeof(info));
    sceFontGetFontInfo(g_font, &info);
    SceFontCharInfo m;
    memset(&m, 0, sizeof(m));
    if (sceFontGetCharInfo(g_font, 'M', &m) < 0 || m.bitmapHeight == 0) {
        l_error("[subtitles] the font has no 'M'");
        sceFontClose(g_font);
        g_font = NULL;
        return false;
    }
    g_cap_px = (float)m.bitmapHeight;
    g_ascent_px = info.maxGlyphAscenderF;
    if (!(g_ascent_px > g_cap_px * 0.9f && g_ascent_px < g_cap_px * 2.f))
        g_ascent_px = g_cap_px * 1.1f;
    g_line_px = info.maxGlyphHeightF;
    if (!(g_line_px > g_cap_px * 1.1f && g_line_px < g_cap_px * 3.f))
        g_line_px = g_cap_px * 1.5f;
    l_info("[subtitles] PGF font %d \"%s\" %.1fx%.1f pt at %.0fx%.0f dpi: 'M' bitmap %ux%u, "
           "ascender %.1f, height %.1f -> ascent %.1f, line %.1f (unscaled px)",
           index, info.fontStyle.fontName, info.fontStyle.fontH, info.fontStyle.fontV,
           info.fontStyle.fontHRes, info.fontStyle.fontVRes, m.bitmapWidth, m.bitmapHeight,
           info.maxGlyphAscenderF, info.maxGlyphHeightF, g_ascent_px, g_line_px);
    g_font_failed = false;
    return true;
}

static const char SUB_VS[] =
    "uniform float2 off;"
    "void main(float2 pos, float2 uv,"
    "          float2 out vUv : TEXCOORD0, float4 out vPos : POSITION)"
    "{ vPos = float4(pos + off, 0.0f, 1.0f); vUv = uv; }";

static const char SUB_FS[] =
    "uniform sampler2D tex; uniform float4 col;"
    "float4 main(float2 vUv : TEXCOORD0) : COLOR { return col * tex2D(tex, vUv); }";

static GLuint compile(GLenum type, const char *src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512] = "";
        glGetShaderInfoLog(s, sizeof(log), NULL, log);
        l_error("[subtitles] shader compile failed: %s", log);
        glDeleteShader(s);
        return 0;
    }
    return s;
}

static bool gl_setup(void) {
    if (g_gl_ready)
        return true;
    if (g_gl_failed)
        return false;
    g_gl_failed = true;

    GLuint vs = compile(GL_CG_VERTEX_SHADER_EXT, SUB_VS);
    GLuint fs = compile(GL_CG_FRAGMENT_SHADER_EXT, SUB_FS);
    if (!vs || !fs)
        return false;
    g_program = glCreateProgram();
    glAttachShader(g_program, vs);
    glAttachShader(g_program, fs);
    glBindAttribLocation(g_program, 0, "pos");
    glBindAttribLocation(g_program, 1, "uv");
    glLinkProgram(g_program);
    GLint ok = 0;
    glGetProgramiv(g_program, GL_LINK_STATUS, &ok);
    if (!ok) {
        l_error("[subtitles] program link failed");
        return false;
    }
    g_u_tex = glGetUniformLocation(g_program, "tex");
    g_u_col = glGetUniformLocation(g_program, "col");
    g_u_off = glGetUniformLocation(g_program, "off");

    GLint prev_tex = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev_tex);
    glGenTextures(1, &g_atlas);
    glBindTexture(GL_TEXTURE_2D, g_atlas);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, ATLAS_W, ATLAS_H, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, (GLuint)prev_tex);
    g_gl_ready = true;
    g_gl_failed = false;
    return true;
}

static glyph_t *glyph_get(uint32_t cp) {
    for (int i = 0; i < g_nglyphs; i++)
        if (g_glyphs[i].cp == cp)
            return &g_glyphs[i];
    if (g_nglyphs == MAX_GLYPHS) {
        if (!g_atlas_full_warned)
            l_warn("[subtitles] glyph table full (%d); later characters are dropped", MAX_GLYPHS);
        g_atlas_full_warned = true;
        return NULL;
    }
    SceFontCharInfo ci;
    memset(&ci, 0, sizeof(ci));
    if (sceFontGetCharInfo(g_font, cp, &ci) < 0) {
        l_warn("[subtitles] no glyph for U+%04X", (unsigned)cp);
        return NULL;
    }
    glyph_t g;
    memset(&g, 0, sizeof(g));
    g.cp = cp;
    g.w = (int)ci.bitmapWidth;
    g.h = (int)ci.bitmapHeight;
    g.left = (int)ci.bitmapLeft;
    g.top = (int)ci.bitmapTop;
    g.adv = (float)ci.sfp26AdvanceH / 64.f;

    if (g.w > 0 && g.h > 0) {
        int pw = g.w + 2 * ATLAS_PAD, ph = g.h + 2 * ATLAS_PAD;
        if (g_shelf_x + pw > ATLAS_W) {
            g_shelf_x = 0;
            g_shelf_y += g_shelf_h;
            g_shelf_h = 0;
        }
        if (g_shelf_y + ph > ATLAS_H) {
            if (!g_atlas_full_warned)
                l_warn("[subtitles] atlas full at %d glyphs; later characters are dropped", g_nglyphs);
            g_atlas_full_warned = true;
            return NULL;
        }
        uint8_t *cov = calloc((size_t)g.w * g.h, 1);
        uint8_t *rgba = calloc((size_t)pw * ph, 4);
        if (!cov || !rgba) {
            free(cov);
            free(rgba);
            return NULL;
        }
        SceFontGlyphImage img;
        memset(&img, 0, sizeof(img));
        img.pixelFormat = SCE_FONT_PIXELFORMAT_8;
        img.xPos64 = 0;
        img.yPos64 = 0;
        img.bufWidth = (unsigned short)g.w;
        img.bufHeight = (unsigned short)g.h;
        img.bytesPerLine = (unsigned short)g.w;
        img.bufferPtr = (unsigned int)(uintptr_t)cov;
        int ret = sceFontGetCharGlyphImage(g_font, cp, &img);
        if (ret < 0)
            l_warn("[subtitles] sceFontGetCharGlyphImage(U+%04X) = %#x", (unsigned)cp, ret);
        for (int y = 0; y < g.h; y++) {
            for (int x = 0; x < g.w; x++) {
                uint8_t *d = rgba + (((size_t)(y + ATLAS_PAD) * pw) + (x + ATLAS_PAD)) * 4;
                d[0] = d[1] = d[2] = 255;
                d[3] = cov[(size_t)y * g.w + x];
            }
        }
        GLint prev_tex = 0;
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev_tex);
        glBindTexture(GL_TEXTURE_2D, g_atlas);
        glTexSubImage2D(GL_TEXTURE_2D, 0, g_shelf_x, g_shelf_y, pw, ph, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
        glBindTexture(GL_TEXTURE_2D, (GLuint)prev_tex);
        free(cov);
        free(rgba);
        g.u0 = (float)(g_shelf_x + ATLAS_PAD) / ATLAS_W;
        g.v0 = (float)(g_shelf_y + ATLAS_PAD) / ATLAS_H;
        g.u1 = g.u0 + (float)g.w / ATLAS_W;
        g.v1 = g.v0 + (float)g.h / ATLAS_H;
        g_shelf_x += pw;
        if (ph > g_shelf_h)
            g_shelf_h = ph;
    }
    g_glyphs[g_nglyphs] = g;
    return &g_glyphs[g_nglyphs++];
}

static const char *utf8_next(const char *s, uint32_t *cp) {
    unsigned char c = (unsigned char)*s;
    int n;
    if (c < 0x80) { *cp = c; return s + 1; }
    if ((c & 0xE0) == 0xC0) { *cp = c & 0x1F; n = 1; }
    else if ((c & 0xF0) == 0xE0) { *cp = c & 0x0F; n = 2; }
    else if ((c & 0xF8) == 0xF0) { *cp = c & 0x07; n = 3; }
    else { *cp = '?'; return s + 1; }
    s++;
    for (int i = 0; i < n; i++, s++) {
        if ((*s & 0xC0) != 0x80) { *cp = '?'; return s; }
        *cp = (*cp << 6) | (uint32_t)(*s & 0x3F);
    }
    return s;
}

static float advance_of(uint32_t cp, float scale) {
    const glyph_t *g = glyph_get(cp);
    return g ? g->adv * scale : 0.f;
}

typedef struct { int start, end; float width; } line_t;

static int layout_lines(const uint32_t *cps, int n, float scale, float max_w, line_t *lines) {
    int nlines = 0, i = 0;
    while (i <= n && nlines < MAX_LINES) {
        int para_end = i;
        while (para_end < n && cps[para_end] != '\r' && cps[para_end] != '\n')
            para_end++;
        int ls = i;
        do {
            while (ls < para_end && cps[ls] == ' ')
                ls++;
            float width = 0.f;
            int j = ls, last_space = -1;
            float width_at_space = 0.f;
            while (j < para_end) {
                float adv = advance_of(cps[j], scale);
                if (width + adv > max_w && j > ls) {
                    if (last_space >= 0) {
                        j = last_space;
                        width = width_at_space;
                    }
                    break;
                }
                width += adv;
                j++;
                if (cps[j - 1] == ' ') {
                    last_space = j - 1;
                    width_at_space = width - adv;
                }
            }
            int le = j;
            while (le > ls && cps[le - 1] == ' ')
                le--;
            lines[nlines].start = ls;
            lines[nlines].end = le;
            lines[nlines].width = width;
            nlines++;
            ls = j;
        } while (ls < para_end && nlines < MAX_LINES);
        if (para_end >= n)
            break;
        i = para_end + 1;
        if (cps[para_end] == '\r' && i < n && cps[i] == '\n')
            i++;
    }
    return nlines;
}

static void emit_quad(float *pos, float *uv, float x0, float y0, float x1, float y1, const glyph_t *g) {
    float cx0 = x0 / SCREEN_W * 2.f - 1.f, cx1 = x1 / SCREEN_W * 2.f - 1.f;
    float cy0 = 1.f - y0 / SCREEN_H * 2.f, cy1 = 1.f - y1 / SCREEN_H * 2.f;
    const float p[12] = { cx0, cy0, cx1, cy0, cx0, cy1, cx1, cy0, cx1, cy1, cx0, cy1 };
    const float t[12] = { g->u0, g->v0, g->u1, g->v0, g->u0, g->v1, g->u1, g->v0, g->u1, g->v1, g->u0, g->v1 };
    memcpy(pos, p, sizeof(p));
    memcpy(uv, t, sizeof(t));
}

long long subtitles_create(const char *utf8, const char *font, unsigned size,
                           const char *align, float x, float y, float w, float h) {
    static bool font_name_noted;
    if (!font_name_noted && font && strcasecmp(font, "Arial") != 0)
        l_info("[subtitles] style asks for font \"%s\"; the system font is used for every style", font);
    font_name_noted = true;

    unsigned px = (unsigned)((float)size * SP_TO_PX + 0.5f);
    if (px == 0)
        px = 1;
    if (!font_setup(px) || !gl_setup())
        return -1;

    sub_t *s = NULL;
    for (int i = 0; i < MAX_LIVE; i++)
        if (g_live[i].id == 0) { s = &g_live[i]; break; }
    if (!s) {
        l_warn("[subtitles] %d subtitles live; dropping a new one", MAX_LIVE);
        return -1;
    }

    uint32_t cps[MAX_CHARS] = { 0 };
    int n = 0;
    for (const char *p = utf8 ? utf8 : ""; *p && n < MAX_CHARS;)
        p = utf8_next(p, &cps[n++]);

    float scale = (CAP_HEIGHT_EM * (float)px) / g_cap_px;
    float bx = x * SCREEN_W, by = y * SCREEN_H, bw = w * SCREEN_W, bh = h * SCREEN_H;
    line_t lines[MAX_LINES];
    int nlines = layout_lines(cps, n, scale, bw, lines);
    float line_h = g_line_px * scale, ascent = g_ascent_px * scale;
    float block_h = (float)nlines * line_h;

    bool top = strncasecmp(align, "Top", 3) == 0, bottom = strncasecmp(align, "Bottom", 6) == 0;
    const char *horiz = top ? align + 3 : bottom ? align + 6 : strncasecmp(align, "Middle", 6) == 0 ? align + 6 : align;
    bool centre = strncasecmp(horiz, "Centre", 6) == 0 || strncasecmp(horiz, "Center", 6) == 0;
    bool right = strncasecmp(horiz, "Right", 5) == 0;
    float y0 = top ? by : bottom ? by + bh - block_h : by + (bh - block_h) * 0.5f;

    int cap = 0;
    for (int l = 0; l < nlines; l++)
        cap += lines[l].end - lines[l].start;
    s->pos = malloc((size_t)cap * 12 * sizeof(float));
    s->uv = malloc((size_t)cap * 12 * sizeof(float));
    if (!s->pos || !s->uv) {
        free(s->pos);
        free(s->uv);
        s->pos = s->uv = NULL;
        return -1;
    }
    int nq = 0;
    for (int l = 0; l < nlines; l++) {
        float pen = right ? bx + bw - lines[l].width : centre ? bx + (bw - lines[l].width) * 0.5f : bx;
        float baseline = y0 + (float)l * line_h + ascent;
        for (int k = lines[l].start; k < lines[l].end; k++) {
            const glyph_t *g = glyph_get(cps[k]);
            if (!g)
                continue;
            if (g->w > 0 && g->h > 0) {
                float gx = pen + (float)g->left * scale, gy = baseline - (float)g->top * scale;
                emit_quad(s->pos + nq * 12, s->uv + nq * 12, gx, gy,
                          gx + (float)g->w * scale, gy + (float)g->h * scale, g);
                nq++;
            }
            pen += g->adv * scale;
        }
    }
    s->nquads = nq;
    s->id = g_next_id++;
    s->col[0] = s->col[1] = s->col[2] = 1.f;
    s->col[3] = 0.f;

    char shown[96];
    int o = 0;
    for (const char *p = utf8 ? utf8 : ""; *p && o < (int)sizeof(shown) - 4; p++)
        shown[o++] = (*p == '\r' || *p == '\n') ? '|' : *p;
    shown[o] = 0;
    l_info("[subtitles] #%lld size %u -> %u px (scale %.2f) %s box (%.2f,%.2f %.2fx%.2f): "
           "%d chars, %d lines, %d quads: \"%s%s\"",
           s->id, size, px, scale, align, x, y, w, h, n, nlines, nq, shown,
           utf8 && strlen(utf8) > (size_t)o ? "..." : "");
    return s->id;
}

static sub_t *find(long long id) {
    if (id <= 0)
        return NULL;
    for (int i = 0; i < MAX_LIVE; i++)
        if (g_live[i].id == id)
            return &g_live[i];
    return NULL;
}

void subtitles_set_colour(long long id, float r, float g, float b, float a) {
    sub_t *s = find(id);
    if (!s)
        return;
    s->col[0] = r;
    s->col[1] = g;
    s->col[2] = b;
    s->col[3] = a;
}

static void drop(sub_t *s) {
    free(s->pos);
    free(s->uv);
    memset(s, 0, sizeof(*s));
}

void subtitles_remove(long long id) {
    sub_t *s = find(id);
    if (s) {
        l_info("[subtitles] #%lld removed", id);
        drop(s);
    }
}

void subtitles_clear(void) {
    for (int i = 0; i < MAX_LIVE; i++)
        if (g_live[i].id)
            drop(&g_live[i]);
}

void subtitles_draw(void) {
    if (!g_gl_ready)
        return;
    bool any = false;
    for (int i = 0; i < MAX_LIVE; i++)
        if (g_live[i].id && g_live[i].nquads && g_live[i].col[3] > 0.f)
            any = true;
    if (!any)
        return;

    GLint src_rgb, dst_rgb, src_a, dst_a, eq;
    glGetIntegerv(GL_BLEND_SRC_RGB, &src_rgb);
    glGetIntegerv(GL_BLEND_DST_RGB, &dst_rgb);
    glGetIntegerv(GL_BLEND_SRC_ALPHA, &src_a);
    glGetIntegerv(GL_BLEND_DST_ALPHA, &dst_a);
    glGetIntegerv(GL_BLEND_EQUATION, &eq);
    GLboolean blend = glIsEnabled(GL_BLEND);

    glEnable(GL_BLEND);
    glBlendEquation(GL_FUNC_ADD);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glUseProgram(g_program);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, g_atlas);
    glUniform1i(g_u_tex, 0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    for (int i = 0; i < MAX_LIVE; i++) {
        sub_t *s = &g_live[i];
        if (!s->id || !s->nquads || s->col[3] <= 0.f)
            continue;
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, s->pos);
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 0, s->uv);
        glUniform2f(g_u_off, SHADOW_PX * 2.f / SCREEN_W, -SHADOW_PX * 2.f / SCREEN_H);
        glUniform4f(g_u_col, 0.f, 0.f, 0.f, s->col[3] * 0.8f);
        glDrawArrays(GL_TRIANGLES, 0, s->nquads * 6);
        glUniform2f(g_u_off, 0.f, 0.f);
        glUniform4f(g_u_col, s->col[0], s->col[1], s->col[2], s->col[3]);
        glDrawArrays(GL_TRIANGLES, 0, s->nquads * 6);
    }

    glBlendFuncSeparate((GLenum)src_rgb, (GLenum)dst_rgb, (GLenum)src_a, (GLenum)dst_a);
    glBlendEquation((GLenum)eq);
    if (!blend)
        glDisable(GL_BLEND);
}
