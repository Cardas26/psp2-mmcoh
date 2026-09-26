#ifndef SOLOADER_IOCACHE_H
#define SOLOADER_IOCACHE_H

#include <stdbool.h>
#include <sys/stat.h>

bool iocache_stat(const char * path, struct stat * st, int * res);

void iocache_invalidate(const char * path);

bool iocache_size(const char * path, uint32_t * size_out);

void iocache_emit_report(void);

#endif
