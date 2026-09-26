#ifndef SOLOADER_BUILD_ID_H
#define SOLOADER_BUILD_ID_H

#ifdef __cplusplus
extern "C" {
#endif

extern const char g_build_id[];

void build_id_log(void);

#ifdef __cplusplus
};
#endif

#endif
