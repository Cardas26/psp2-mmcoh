#include "filelog.h"

#ifndef ZERO_INTERFERENCE

#include "utils/lazy_lwmutex.h"
#include "utils/watchdog.h"

#include <stdio.h>
#include <psp2/kernel/processmgr.h>

#define LOG_FILE_PATH DATA_PATH "debug.log"

static lazy_lwmutex_t log_write_mutex = LAZY_LWMUTEX_INITIALIZER;

static FILE *log_file = NULL;

void file_log_write(const char *line) {
    if (!lazy_lwmutex_lock(&log_write_mutex, "filelog_lock"))
        return;

    if (!log_file) {
        log_file = fopen(LOG_FILE_PATH, "w");
        if (!log_file) {
            lazy_lwmutex_unlock(&log_write_mutex);
            return;
        }
    }
    uint64_t us = sceKernelGetProcessTimeWide();
    fprintf(log_file, "[%6llu.%03llu] ",
            (unsigned long long)(us / 1000000ULL),
            (unsigned long long)((us / 1000ULL) % 1000ULL));
    fputs(line, log_file);
    fflush(log_file);

    lazy_lwmutex_unlock(&log_write_mutex);

    watchdog_note_log_activity();
}

#endif
