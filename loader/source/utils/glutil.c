/*
 * Copyright (C) 2021      Andy Nguyen
 * Copyright (C) 2021      Rinnegatamante
 * Copyright (C) 2022-2023 Volodymyr Atamanenko
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include "utils/glutil.h"

#include "utils/utils.h"
#include "utils/dialog.h"
#include "utils/logger.h"
#include "utils/shader_cache.h"
#include "utils/boundary_census.h"
#include <stdio.h>
#include <malloc.h>
#include <string.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/io/stat.h>

GLboolean skip_next_compile = GL_FALSE;
static char next_shader_fname[256];
void load_shader(GLuint shader, const char * string, size_t length);

void gl_preload() {
    if (!file_exists("ur0:/data/libshacccg.suprx")
        && !file_exists("ur0:/data/external/libshacccg.suprx")) {
        fatal_error("Error: libshacccg.suprx is not installed. "
                    "Google \"ShaRKBR33D\" for quick installation.");
    }

    vglSetSemanticBindingMode(VGL_MODE_POSTPONED);
}

#define VGL_RAM_THRESHOLD (32 * 1024 * 1024)
#define VGL_PHYCONT_THRESHOLD (20 * 1024 * 1024)

static int gl_ready = 0;

int gl_is_ready() {
    return gl_ready;
}

void gl_init() {
    if (gl_ready)
        return;

    shader_cache_prepare();

    vglInitWithCustomThreshold(0, SCREEN_NATIVE_W, SCREEN_NATIVE_H,
                               VGL_RAM_THRESHOLD, 0, VGL_PHYCONT_THRESHOLD,
                               0x8C6000, SCE_GXM_MULTISAMPLE_NONE);
    gl_ready = 1;

    shader_cache_commit();
}

void gl_swap() {
    vglSwapBuffers(GL_FALSE);
}

void glShaderSource_soloader(GLuint shader, GLsizei count,
                             const GLchar **string, const GLint *_length) {
    if (!string) {
        l_error("<%p> Shader source string is NULL, count: %i",
                   __builtin_return_address(0), count);
        skip_next_compile = GL_TRUE;
        return;
    } else if (!*string) {
        l_error("<%p> Shader source *string is NULL, count: %i",
                   __builtin_return_address(0), count);
        skip_next_compile = GL_TRUE;
        return;
    }

    size_t total_length = 0;

    for (int i = 0; i < count; ++i) {
        if (!_length || _length[i] < 0) {
            total_length += strlen(string[i]);
        } else {
            total_length += (size_t)_length[i];
        }
    }

    char * str = malloc(total_length+1);
    if (!str) {
        l_error("<%p> could not allocate %u bytes of shader source",
                __builtin_return_address(0), (unsigned)(total_length + 1));
        skip_next_compile = GL_TRUE;
        return;
    }
    size_t l = 0;

    for (int i = 0; i < count; ++i) {
        if (!_length || _length[i] < 0) {
            memcpy(str + l, string[i], strlen(string[i]));
            l += strlen(string[i]);
        } else {
            memcpy(str + l, string[i], (size_t)_length[i]);
            l += (size_t)_length[i];
        }
    }
    str[total_length] = '\0';

    load_shader(shader, str, total_length);

    free(str);
}

void glCompileShader_soloader(GLuint shader) {

    if (!skip_next_compile) {
        shader_cache_time_begin();
        glCompileShader(shader);
        shader_cache_time_end("compile");
    }
    skip_next_compile = GL_FALSE;
}

void glLinkProgram_soloader(GLuint program) {
    shader_cache_time_begin();
    glLinkProgram(program);
    shader_cache_time_end("link");
    shader_cache_report();
}

void load_shader(GLuint shader, const char * string, size_t length) {
    glShaderSource(shader, 1, &string, &length);
}
