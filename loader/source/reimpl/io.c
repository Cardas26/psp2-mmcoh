/*
 * Copyright (C) 2021      Andy Nguyen
 * Copyright (C) 2022      Rinnegatamante
 * Copyright (C) 2022-2024 Volodymyr Atamanenko
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include "reimpl/io.h"
#include "reimpl/iocache.h"
#include "reimpl/iocontent.h"
#include "reimpl/iopath.h"

extern void * mmap(void * addr, size_t length, int prot, int flags, int fd, off_t offset);
extern int munmap(void * addr, size_t length);

#include <errno.h>
#include <stdbool.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/unistd.h>
#include <stdlib.h>
#include <dirent.h>
#include <stdarg.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>

#include "utils/lazy_lwmutex.h"
#include "utils/logger.h"
#include "utils/update_profile.h"
#include "audio/music_seg_cache.h"
#include "utils/utils.h"

#include "reimpl/bits/_struct_converters.c"

#include <stddef.h>
_Static_assert(offsetof(struct __sFILE, _file) == 14,
               "newlib __sFILE::_file is no longer at bionic's offset 14 - "
               "MoFlow reads FILE::_file directly, see reimpl/io.c");
_Static_assert(sizeof(((struct __sFILE *)0)->_file) == 2,
               "newlib __sFILE::_file is no longer 16-bit");

#define io_trace(...) do {} while (0)

#define IO_PERF_TIMED(kind, bytes, call) (call)

_Static_assert(BIONIC_SFILE_SIZE <= sizeof(FILE),
               "BIONIC_SFILE_SIZE grew past vitasdk's own FILE size");
void fixup_bionic_stdio(uint8_t sf_fake[3][BIONIC_SFILE_SIZE]) {
    static FILE * out;
    if (!out) {
        out = fopen(DATA_PATH "stdio_redirect.log", "w");
        if (out && setvbuf(out, NULL, _IONBF, 0) != 0)
            l_warn("fixup_bionic_stdio: setvbuf(_IONBF) failed - a terminate "
                   "message may be lost in the buffer when abort() kills us");
        if (!out)
            l_warn("fixup_bionic_stdio: fopen(stdio_redirect.log) failed - the "
                   "game's stdout/stderr stay unreadable");
    }
    const FILE * sink = out ? out : stderr;
    memcpy(sf_fake[0], stdin, BIONIC_SFILE_SIZE);
    memcpy(sf_fake[1], sink, BIONIC_SFILE_SIZE);
    memcpy(sf_fake[2], sink, BIONIC_SFILE_SIZE);
}

#define BANK_STDIO_BUFFER (256 * 1024)

static FILE * g_gamelog;
static bool   g_gamelog_busy;
static lazy_lwmutex_t g_gamelog_lock = LAZY_LWMUTEX_INITIALIZER;

static bool is_game_log(const char * path, const char * mode) {
    return path && mode && mode[0] == 'a' && !strchr(mode, '+') &&
           strstr(path, "MoFloLog.txt") != NULL;
}

static FILE * gamelog_take(const char * path, const char * mode) {
    if (!is_game_log(path, mode))
        return NULL;
    if (!lazy_lwmutex_lock(&g_gamelog_lock, "game_log"))
        return NULL;
    if (g_gamelog_busy) {
        lazy_lwmutex_unlock(&g_gamelog_lock);
        return NULL;
    }
    if (!g_gamelog)
        g_gamelog = fopen(path, mode);
    FILE * f = g_gamelog;
    if (f) {
        g_gamelog_busy = true;
    }
    lazy_lwmutex_unlock(&g_gamelog_lock);
    return f;
}

static bool gamelog_give(FILE * f) {
    if (!f || f != g_gamelog)
        return false;
    if (!lazy_lwmutex_lock(&g_gamelog_lock, "game_log"))
        return false;
    bool ours = g_gamelog_busy && f == g_gamelog;
    if (ours) {
        fflush(f);
        g_gamelog_busy = false;
    }
    lazy_lwmutex_unlock(&g_gamelog_lock);
    return ours;
}

FILE * fopen_soloader(const char * filename, const char * mode) {
    if (strcmp(filename, "/proc/cpuinfo") == 0) {
        return fopen_soloader("app0:/cpuinfo", mode);
    } else if (strcmp(filename, "/proc/meminfo") == 0) {
        return fopen_soloader("app0:/meminfo", mode);
    }

    IO_SHORT(path, filename);

    FILE * served = iocontent_take_handle(path, mode);
    if (!served)
        served = gamelog_take(path, mode);
    if (served) {
        io_trace("fopen(%s, %s): %p fd=%d [served]", path, mode, served,
                 fileno(served));
        return served;
    }

    FILE* ret = fopen(path, mode);
    if (ret && strstr(path, "SoundBanks/") && setvbuf(ret, NULL, _IOFBF, BANK_STDIO_BUFFER) != 0)
        l_warn("fopen(%s): setvbuf(%u) failed - reading through the default buffer", path, (unsigned) BANK_STDIO_BUFFER);

    if (ret) {
        if (mode[0] == 'r' && !strchr(mode, '+'))
            iocontent_open(path, fileno(ret));
        music_seg_cache_bank_fopen(ret, path);
    } else {
        l_warn("fopen(%s, %s): %p", path, mode, ret);
    }

    return ret;
}

int chdir_soloader(const char * path) {
    IO_SHORT(p, path);
    int ret = chdir(p);
    if (ret == 0)
        io_trace("chdir(%s): %d", p, ret);
    else
        l_warn("chdir(%s): %d", p, ret);
    return ret;
}

int open_soloader(const char * path, int oflag, ...) {
    if (strcmp(path, "/proc/cpuinfo") == 0) {
        return open_soloader("app0:/cpuinfo", oflag);
    } else if (strcmp(path, "/proc/meminfo") == 0) {
        return open_soloader("app0:/meminfo", oflag);
    } else if (strcmp(path, "/dev/urandom") == 0) {
        return open_soloader("app0:/urandom", oflag);
    }

    mode_t mode = 0666;
    if (((oflag & BIONIC_O_CREAT) == BIONIC_O_CREAT) ||
        ((oflag & BIONIC_O_TMPFILE) == BIONIC_O_TMPFILE)) {
        va_list args;
        va_start(args, oflag);
        mode = (mode_t)(va_arg(args, int));
        va_end(args);
    }

    oflag = oflags_bionic_to_newlib(oflag);
    IO_SHORT(p, path);
    if (oflag & O_CREAT)
        iocache_invalidate(p);
    iocontent_invalidate(p);
    int ret = open(p, oflag, mode);
    if (ret >= 0) {
        if ((oflag & O_ACCMODE) == O_RDONLY)
            iocontent_open(p, ret);
    } else {
        l_warn("open(%s, %x): %i", p, oflag, ret);
    }
    return ret;
}

int fstat_soloader(int fd, stat64_bionic * buf) {
    struct stat st;
    int res = fstat(fd, &st);

    if (res == 0)
        stat_newlib_to_bionic(&st, buf);

    return res;
}

int stat_soloader(const char * path, stat64_bionic * buf) {
    if (strcmp(path, "/system/lib/libOpenSLES.so") == 0) {

        if (buf) {
            memset(buf, 0, sizeof(*buf));
            buf->st_mode = S_IFREG | 0444;
            buf->st_nlink = 1;
            buf->st_size = 1;
            buf->st_blksize = 512;
        }
        return 0;
    }

    IO_SHORT(p, path);
    struct stat st;
    int res;

    if (iocache_stat(p, &st, &res)) {
    } else {
        res = IO_PERF_TIMED(IOPERF_STAT, 0, stat(p, &st));
    }

    if (res == 0)
        stat_newlib_to_bionic(&st, buf);

    return res;
}

int fclose_soloader(FILE * f) {
    if (iocontent_give_handle(f) || gamelog_give(f)) {
        return 0;
    }
    if (f)
        iocontent_close(fileno(f));
    music_seg_cache_bank_fclose(f);
    int ret = fclose(f);

    return ret;
}

size_t fread_soloader(void * ptr, size_t size, size_t count, FILE * stream) {
    size_t ret;
    size_t bytes = size * count;
    if (bytes && size == 1 && music_seg_cache_is_bank(stream)) {
        long pos = ftell(stream);
        size_t got = music_seg_cache_read(stream, pos, ptr, bytes);
        if (got == bytes) {
            fseek(stream, pos + (long)got, SEEK_SET);
            ret = count;
        } else {
            ret = fread(ptr, size, count, stream);
        }
    } else {
        ret = fread(ptr, size, count, stream);
    }
    return ret;
}

off_t lseek_soloader(int fd, off_t offset, int whence) {
    off_t cached;
    if (iocontent_seek(fd, offset, whence, &cached)) {
        return cached;
    }
    off_t ret = io_lseek_timed(fd, offset, whence);
    return ret;
}

ssize_t io_read_timed(int fd, void * buf, size_t count) {
    ssize_t ret = read(fd, buf, count);
    return ret;
}

off_t io_lseek_timed(int fd, off_t offset, int whence) {
    return IO_PERF_TIMED(IOPERF_LSEEK, 0, lseek(fd, offset, whence));
}

ssize_t read_soloader(int fd, void * buf, size_t count) {
    ssize_t cached;
    if (iocontent_read(fd, buf, count, &cached)) {
        return cached;
    }
    ssize_t ret = io_read_timed(fd, buf, count);
    return ret;
}

int close_soloader(int fd) {
    iocontent_close(fd);
    int ret = close(fd);
    return ret;
}

typedef struct {
    DIR * real;
    dirent64_bionic entry;
} bionic_dir;

DIR* opendir_soloader(char* _pathname) {
    IO_SHORT(p, _pathname);

    DIR * real = opendir(p);
    if (!real) {
        return NULL;
    }

    bionic_dir * bd = malloc(sizeof(bionic_dir));
    if (!bd) {
        closedir(real);
        l_error("opendir(\"%s\"): out of memory", p);
        errno = ENOMEM;
        return NULL;
    }

    bd->real = real;
    return (DIR *) bd;
}

int access_soloader(const char * path, int mode) {
    IO_SHORT(p, path);
    int ret = access(p, mode);
    return ret;
}

int chmod_soloader(const char * path, mode_t mode) {
    IO_SHORT(p, path);
    return chmod(p, mode);
}

int chown_soloader(const char * path, int uid, int gid) {
    (void) path; (void) uid; (void) gid;
    return 0;
}

int lstat_soloader(const char * path, stat64_bionic * buf) {
    IO_SHORT(p, path);
    struct stat st;
    int res = lstat(p, &st);

    if (res == 0)
        stat_newlib_to_bionic(&st, buf);

    return res;
}

int mkdir_soloader(const char * path, mode_t mode) {
    IO_SHORT(p, path);
    iocache_invalidate(p);
    iocontent_invalidate(p);
    int ret = mkdir(p, mode);
    return ret;
}

int remove_soloader(const char * path) {
    IO_SHORT(p, path);
    iocache_invalidate(p);
    iocontent_invalidate(p);
    return remove(p);
}

int rename_soloader(const char * from, const char * to) {
    IO_SHORT(f, from);
    IO_SHORT(t, to);
    iocache_invalidate(f);
    iocache_invalidate(t);
    iocontent_invalidate(f);
    iocontent_invalidate(t);
    return rename(f, t);
}

int rmdir_soloader(const char * path) {
    IO_SHORT(p, path);
    iocache_invalidate(p);
    iocontent_invalidate(p);
    return rmdir(p);
}

int unlink_soloader(const char * path) {
    IO_SHORT(p, path);
    iocache_invalidate(p);
    iocontent_invalidate(p);
    return unlink(p);
}

struct dirent64_bionic * readdir_soloader(DIR * dir) {
    if (!dir) {
        errno = EBADF;
        return NULL;
    }

    bionic_dir * bd = (bionic_dir *) dir;

    const struct dirent * ret = readdir(bd->real);

    if (!ret)
        return NULL;

    dirent_newlib_to_bionic(ret, &bd->entry);
    return &bd->entry;
}

int readdir_r_soloader(DIR * dirp, dirent64_bionic * entry,
                       dirent64_bionic ** result) {
    if (!dirp) {
        errno = EBADF;
        return EBADF;
    }

    bionic_dir * bd = (bionic_dir *) dirp;

    struct dirent dirent_tmp;
    struct dirent * pdirent_tmp;

    int ret = readdir_r(bd->real, &dirent_tmp, &pdirent_tmp);

    if (ret == 0) {
        if (pdirent_tmp != NULL)
            dirent_newlib_to_bionic(&dirent_tmp, entry);
        *result = (pdirent_tmp != NULL) ? entry : NULL;
    }

    return ret;
}

int closedir_soloader(DIR * dir) {
    if (!dir) {
        errno = EBADF;
        return -1;
    }

    bionic_dir * bd = (bionic_dir *) dir;

    int ret = closedir(bd->real);

    free(bd);
    return ret;
}

int fcntl_soloader(int fd, int cmd, ...) {
    l_warn("fcntl(%i, %i, ...): not implemented", fd, cmd);
    return 0;
}

int ioctl_soloader(int fd, int request, ...) {
    l_warn("ioctl(%i, %i, ...): not implemented", fd, request);
    return 0;
}

int fsync_soloader(int fd) {
    int ret = fsync(fd);
    return ret;
}
