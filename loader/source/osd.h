#ifndef SOLOADER_OSD_H
#define SOLOADER_OSD_H

#ifdef __cplusplus
extern "C" {
#endif

void osd_say(const char *what, const char *name);

void osd_show(const char *text, int ms);

void osd_queue(const char *text, int ms);

void osd_frame(void);

#ifdef __cplusplus
}
#endif

#endif
