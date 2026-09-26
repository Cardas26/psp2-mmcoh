/*
 * Copyright (C) 2025 Volodymyr Atamanenko
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#ifndef SOLOADER_AUDIO_H
#define SOLOADER_AUDIO_H

#ifdef __cplusplus
extern "C" {
#endif

void audio_pump_start(void *fmod_system);

void audio_pump_stop(void);

#include <stdint.h>

#ifdef __cplusplus
};
#endif

#endif
