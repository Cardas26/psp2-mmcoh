#ifndef SOLOADER_IOCONTENT_H
#define SOLOADER_IOCONTENT_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stddef.h>
#include <sys/types.h>

void iocontent_open(const char * path, int fd);

bool iocontent_read(int fd, void * buf, size_t count, ssize_t * ret);

bool iocontent_seek(int fd, off_t offset, int whence, off_t * ret);

void iocontent_close(int fd);

void iocontent_invalidate(const char * path);

FILE * iocontent_take_handle(const char * path, const char * mode);

bool iocontent_give_handle(FILE * f);

void iocontent_pack_start(void);

#endif
