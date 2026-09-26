#ifndef SOLOADER_SHADER_CACHE_H
#define SOLOADER_SHADER_CACHE_H

#ifdef __cplusplus
extern "C" {
#endif

void shader_cache_prepare(void);

void shader_cache_commit(void);

void shader_cache_time_begin(void);
void shader_cache_time_end(const char * what);

void shader_cache_report(void);

#ifdef __cplusplus
};
#endif

#endif
