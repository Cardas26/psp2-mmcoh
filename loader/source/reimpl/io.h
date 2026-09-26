/*
 * Copyright (C) 2021      Andy Nguyen
 * Copyright (C) 2022      Rinnegatamante
 * Copyright (C) 2022-2024 Volodymyr Atamanenko
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#ifndef SOLOADER_IO_H
#define SOLOADER_IO_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdio.h>
#include <stdarg.h>
#include <stdint.h>
#include <wchar.h>
#include <sys/dirent.h>
#include <sys/syslimits.h>
#include <sys/fcntl.h>

#ifndef PATH_MAX
#define PATH_MAX 1024
#endif

#ifndef DT_DIR
#define DT_UNKNOWN 0
#define DT_FIFO 1
#define DT_CHR 2
#define DT_DIR 4
#define DT_BLK 6
#define DT_REG 8
#define DT_LNK 10
#define DT_SOCK 12
#define DT_WHT 14
#endif

typedef struct __attribute__((__packed__)) stat64_bionic {
    unsigned long long st_dev;
    unsigned char __pad0[4];
    unsigned long __st_ino;
    unsigned int st_mode;
    nlink_t st_nlink;
    uid_t st_uid;
    gid_t st_gid;
    unsigned long long st_rdev;
    unsigned char __pad3[4];
    long long st_size;
    unsigned long st_blksize;
    unsigned long long st_blocks;
    struct timespec st_atim;
    struct timespec st_mtim;
    struct timespec st_ctim;
    unsigned long long st_ino;
} stat64_bionic;

typedef struct __attribute__((__packed__)) dirent64_bionic {
    int16_t d_ino;
    int64_t d_off;
    uint64_t d_reclen;
    unsigned char d_type;
    char d_name[256];
} dirent64_bionic;

int open_soloader(const char * path, int oflag, ...);

FILE * fopen_soloader(const char * filename, const char * mode);
size_t fread_soloader(void * ptr, size_t size, size_t count, FILE * stream);

#define BIONIC_SFILE_SIZE 84

void fixup_bionic_stdio(uint8_t sf_fake[3][BIONIC_SFILE_SIZE]);

DIR *opendir_soloader(char *name);

int stat_soloader(const char * path, stat64_bionic * buf);

int fstat_soloader(int fd, stat64_bionic * buf);

int chdir_soloader(const char * path);

struct dirent64_bionic * readdir_soloader(DIR *dir);

int readdir_r_soloader(DIR * dirp, dirent64_bionic * entry,
                       dirent64_bionic ** result);

int close_soloader(int fd);

int fclose_soloader(FILE *f);

off_t lseek_soloader(int fd, off_t offset, int whence);
ssize_t read_soloader(int fd, void * buf, size_t count);

ssize_t io_read_timed(int fd, void * buf, size_t count);
off_t   io_lseek_timed(int fd, off_t offset, int whence);

int access_soloader(const char * path, int mode);
int chmod_soloader(const char * path, mode_t mode);
int chown_soloader(const char * path, int uid, int gid);
int lstat_soloader(const char * path, stat64_bionic * buf);
int mkdir_soloader(const char * path, mode_t mode);
int remove_soloader(const char * path);
int rename_soloader(const char * from, const char * to);
int rmdir_soloader(const char * path);
int unlink_soloader(const char * path);

int closedir_soloader(DIR *dir);

int fcntl_soloader(int fd, int cmd, ...);

int ioctl_soloader(int fd, int request, ... );

int fsync_soloader(int fd);

#ifdef __cplusplus
};
#endif

#endif
