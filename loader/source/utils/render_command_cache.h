#ifndef SOLOADER_RENDER_COMMAND_CACHE_H
#define SOLOADER_RENDER_COMMAND_CACHE_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#define MAT_SIZE            0x17c
#define MAT_CACHE_VALID     0x168
#define MAT_TEXTURES        0x16c
#define MAT_SHADER_NAME     0x150
#define MAT_MAP_FIRST       0x28
#define MAT_MAP_STRIDE      0x1c
#define MAT_MAP_COUNT       7
#define MAT_MAP_SIZE_OFF    0x0c
#define MAT_MAP_BUCKETS_OFF 0x14
#define MAT_MAP_VALUE_SIZES { 4, 8, 12, 16, 64, 0, 16 }
#define MAP_SNAP_MAX        224

int rcc_maps_serialise(const uint8_t * mat, uint8_t * buf);

void render_command_cache_install(void);

#ifdef __cplusplus
};
#endif

#endif
