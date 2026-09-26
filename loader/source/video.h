#ifndef SOLOADER_VIDEO_H
#define SOLOADER_VIDEO_H

#include <stdbool.h>

void video_present(const char *path, bool in_apk, bool can_dismiss, bool has_subtitles);

void video_dismiss(void);

void video_frame(void);

bool  video_is_playing(void);
float video_time_s(void);
float video_duration_s(void);

#endif
