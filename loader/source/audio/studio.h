#ifndef MMCOH_AUDIO_STUDIO_H
#define MMCOH_AUDIO_STUDIO_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define studio_init()                          (false)
#define studio_play_range(p, o, l, lp)         ((void)(p), (void)(o), (void)(l), (void)(lp), false)
#define studio_is_playing_range(o)             ((void)(o), false)

#ifdef __cplusplus
}
#endif

#endif
