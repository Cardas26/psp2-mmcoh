#include "reimpl/iopath.h"

#include <string.h>

#define ANDROID_PRIVATE_DIR "Android/data/" PKG_NAME "/"
#define ANDROID_CACHE_DIR   "cache/"

const char * io_shorten_path(const char * path, char * buf, size_t buflen) {
    if (!path)
        return path;

    const char * hit = strstr(path, ANDROID_PRIVATE_DIR);
    if (!hit)
        return path;

    const char * rest = hit + sizeof(ANDROID_PRIVATE_DIR) - 1;
    if (strncmp(rest, ANDROID_CACHE_DIR, sizeof(ANDROID_CACHE_DIR) - 1) == 0)
        rest += sizeof(ANDROID_CACHE_DIR) - 1;

    size_t head = (size_t)(hit - path);
    if (head + strlen(rest) + 1 > buflen)
        return path;

    memcpy(buf, path, head);
    memcpy(buf + head, rest, strlen(rest) + 1);
    return buf;
}
