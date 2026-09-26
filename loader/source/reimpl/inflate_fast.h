#ifndef SOLOADER_INFLATE_FAST_H
#define SOLOADER_INFLATE_FAST_H

#include <zlib.h>

#ifdef __cplusplus
extern "C" {
#endif

int inflate_oneshot(z_streamp strm, int flush);

int inflateInit_oneshot(z_streamp strm, const char * version, int stream_size);
int inflateInit2_oneshot(z_streamp strm, int windowBits, const char * version,
                         int stream_size);
int inflateReset_oneshot(z_streamp strm);
int inflateReset2_oneshot(z_streamp strm, int windowBits);
int inflateEnd_oneshot(z_streamp strm);

#ifdef __cplusplus
}
#endif

#endif
