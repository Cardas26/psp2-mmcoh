#ifndef SOLOADER_SUBTITLES_H
#define SOLOADER_SUBTITLES_H

#include <stdbool.h>

long long subtitles_create(const char *utf8, const char *font, unsigned size,
                           const char *align, float x, float y, float w, float h);

void subtitles_set_colour(long long id, float r, float g, float b, float a);

void subtitles_remove(long long id);

void subtitles_clear(void);

void subtitles_draw(void);

#endif
