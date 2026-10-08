#include "osd.h"

#include "utils/lazy_lwmutex.h"
#include "utils/logger.h"
#include "utils/utils.h"

#include <osd_font.h>

#include <psp2/kernel/processmgr.h>
#include <vitaGL.h>

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define OSD_SAY_MS  3000
#define OSD_QUEUE   10
#define OSD_X       8
#define OSD_Y       8
#define OSD_SCALE   2

static lazy_lwmutex_t g_lock = LAZY_LWMUTEX_INITIALIZER;
static char g_text[96];
static int g_ms;
static uint64_t g_until_us;
static unsigned g_gen;
static struct { char text[96]; int ms; } g_queue[OSD_QUEUE];
static int g_q_head, g_q_n;
static atomic_int g_live;

void osd_show(const char *text, int ms) {
    if (!lazy_lwmutex_lock(&g_lock, "osd"))
        return;
    snprintf(g_text, sizeof(g_text), "%s", text);
    g_ms = ms;
    g_until_us = 0;
    g_gen++;
    atomic_store(&g_live, 1);
    lazy_lwmutex_unlock(&g_lock);
    l_info("[osd] %s", text);
}

void osd_say(const char *what, const char *name) {
    char t[96];
    snprintf(t, sizeof(t), "%s: %s", what, name);
    osd_show(t, OSD_SAY_MS);
}

void osd_queue(const char *text, int ms) {
    if (!lazy_lwmutex_lock(&g_lock, "osd"))
        return;
    bool queued = g_q_n < OSD_QUEUE;
    if (queued) {
        int i = (g_q_head + g_q_n++) % OSD_QUEUE;
        snprintf(g_queue[i].text, sizeof(g_queue[i].text), "%s", text);
        g_queue[i].ms = ms;
        atomic_store(&g_live, 1);
    }
    lazy_lwmutex_unlock(&g_lock);
    l_info("[osd] %s%s", queued ? "" : "(queue full, dropped) ", text);
}

static unsigned osd_current(char *out, size_t n) {
    if (!lazy_lwmutex_lock(&g_lock, "osd"))
        return 0;
    uint64_t now = sceKernelGetProcessTimeWide();
    if (g_text[0] && g_until_us && now >= g_until_us) {
        g_text[0] = 0;
        g_gen++;
    }
    if (!g_text[0] && g_q_n) {
        snprintf(g_text, sizeof(g_text), "%s", g_queue[g_q_head].text);
        g_ms = g_queue[g_q_head].ms;
        g_until_us = 0;
        g_q_head = (g_q_head + 1) % OSD_QUEUE;
        g_q_n--;
        g_gen++;
    }
    if (g_text[0] && !g_until_us)
        g_until_us = now + (uint64_t)g_ms * 1000;
    unsigned gen = g_text[0] ? g_gen : 0;
    snprintf(out, n, "%s", g_text);
    if (!g_text[0] && !g_q_n)
        atomic_store(&g_live, 0);
    lazy_lwmutex_unlock(&g_lock);
    return gen;
}

static const char OSD_VS[] =
    "void main(float2 pos, float2 uv,"
    "          float2 out vUv : TEXCOORD0, float4 out vPos : POSITION)"
    "{ vPos = float4(pos, 0.0f, 1.0f); vUv = uv; }";

static const char OSD_FS[] =
    "uniform sampler2D tex;"
    "float4 main(float2 vUv : TEXCOORD0) : COLOR { return tex2D(tex, vUv); }";

static GLuint g_program, g_tex;
static GLint g_uniform_tex;
static int g_gl_state;
static uint32_t g_px[OSD_W * OSD_H];
static int g_used;
static unsigned g_drawn_gen;
static float g_pos[8], g_uv[8];

static GLuint compile(GLenum type, const char *src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512] = "";
        glGetShaderInfoLog(s, sizeof(log), NULL, log);
        l_error("[osd] shader compile failed: %s", log);
        glDeleteShader(s);
        return 0;
    }
    return s;
}

static bool gl_setup(void) {
    if (g_gl_state)
        return g_gl_state > 0;
    g_gl_state = -1;
    GLuint vs = compile(GL_CG_VERTEX_SHADER_EXT, OSD_VS);
    GLuint fs = compile(GL_CG_FRAGMENT_SHADER_EXT, OSD_FS);
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
        l_error("[osd] program link failed");
        return false;
    }
    g_uniform_tex = glGetUniformLocation(g_program, "tex");
    glGenTextures(1, &g_tex);
    g_gl_state = 1;
    return true;
}

static void upload(const char *text) {
    g_used = osd_raster(text, g_px, OSD_W, OSD_H);
    GLint prev_tex = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev_tex);
    glBindTexture(GL_TEXTURE_2D, g_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, OSD_W, OSD_H, 0, GL_RGBA, GL_UNSIGNED_BYTE, g_px);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, (GLuint)prev_tex);

    float x0 = -1.f + 2.f * OSD_X / SCREEN_NATIVE_W;
    float x1 = -1.f + 2.f * (OSD_X + g_used * OSD_SCALE) / SCREEN_NATIVE_W;
    float y0 = 1.f - 2.f * OSD_Y / SCREEN_NATIVE_H;
    float y1 = 1.f - 2.f * (OSD_Y + OSD_H * OSD_SCALE) / SCREEN_NATIVE_H;
    float u1 = (float)g_used / OSD_W;
    const float pos[8] = { x0, y0,  x1, y0,  x0, y1,  x1, y1 };
    const float uv[8]  = { 0.f, 0.f,  u1, 0.f,  0.f, 1.f,  u1, 1.f };
    memcpy(g_pos, pos, sizeof(g_pos));
    memcpy(g_uv, uv, sizeof(g_uv));
}

static void draw(void) {
    GLint prev_program, prev_active, prev_tex, prev_array_buf, prev_viewport[4];
    GLint attr0_on, attr1_on, src_rgb, dst_rgb, src_a, dst_a, eq;
    glGetIntegerv(GL_CURRENT_PROGRAM, &prev_program);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &prev_active);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &prev_array_buf);
    glGetIntegerv(GL_VIEWPORT, prev_viewport);
    glGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &attr0_on);
    glGetVertexAttribiv(1, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &attr1_on);
    glGetIntegerv(GL_BLEND_SRC_RGB, &src_rgb);
    glGetIntegerv(GL_BLEND_DST_RGB, &dst_rgb);
    glGetIntegerv(GL_BLEND_SRC_ALPHA, &src_a);
    glGetIntegerv(GL_BLEND_DST_ALPHA, &dst_a);
    glGetIntegerv(GL_BLEND_EQUATION, &eq);
    GLboolean blend = glIsEnabled(GL_BLEND), depth = glIsEnabled(GL_DEPTH_TEST),
              scissor = glIsEnabled(GL_SCISSOR_TEST), cull = glIsEnabled(GL_CULL_FACE);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev_tex);

    glEnable(GL_BLEND);
    glBlendEquation(GL_FUNC_ADD);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_CULL_FACE);
    glViewport(0, 0, SCREEN_NATIVE_W, SCREEN_NATIVE_H);
    glUseProgram(g_program);
    glBindTexture(GL_TEXTURE_2D, g_tex);
    glUniform1i(g_uniform_tex, 0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, g_pos);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 0, g_uv);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    if (!attr0_on) glDisableVertexAttribArray(0);
    if (!attr1_on) glDisableVertexAttribArray(1);
    glBindBuffer(GL_ARRAY_BUFFER, (GLuint)prev_array_buf);
    glBindTexture(GL_TEXTURE_2D, (GLuint)prev_tex);
    glActiveTexture((GLenum)prev_active);
    glUseProgram((GLuint)prev_program);
    glViewport(prev_viewport[0], prev_viewport[1], prev_viewport[2], prev_viewport[3]);
    glBlendFuncSeparate((GLenum)src_rgb, (GLenum)dst_rgb, (GLenum)src_a, (GLenum)dst_a);
    glBlendEquation((GLenum)eq);
    if (!blend) glDisable(GL_BLEND);
    if (depth) glEnable(GL_DEPTH_TEST);
    if (scissor) glEnable(GL_SCISSOR_TEST);
    if (cull) glEnable(GL_CULL_FACE);
}

void osd_frame(void) {
    if (!atomic_load_explicit(&g_live, memory_order_relaxed))
        return;
    char text[96];
    unsigned gen = osd_current(text, sizeof(text));
    if (!gen || !gl_setup())
        return;
    if (gen != g_drawn_gen) {
        upload(text);
        g_drawn_gen = gen;
    }
    if (g_used)
        draw();
}
