#ifndef MMCOH_AUDIO_MUSIC_SEG_CACHE_H
#define MMCOH_AUDIO_MUSIC_SEG_CACHE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

void music_seg_cache_bank_fopen(FILE *f, const char *path);
void music_seg_cache_bank_fclose(FILE *f);
bool music_seg_cache_is_bank(FILE *f);

size_t music_seg_cache_read(FILE *f, long pos, void *ptr, size_t bytes);

void music_seg_cache_hint(int bank);

#ifdef __cplusplus
}
#endif

#endif
