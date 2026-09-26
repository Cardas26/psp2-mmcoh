#ifndef SOLOADER_IOPATH_H
#define SOLOADER_IOPATH_H

#include <stddef.h>

#define SOLOADER_PATH_MAX 1024

const char * io_shorten_path(const char * path, char * buf, size_t buflen);

#define IO_SHORT(var, path)                                                    \
    char var##_buf[SOLOADER_PATH_MAX];                                         \
    const char * var = io_shorten_path((path), var##_buf, sizeof(var##_buf))

#endif
