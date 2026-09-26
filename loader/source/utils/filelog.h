#ifndef SOLOADER_FILELOG_H
#define SOLOADER_FILELOG_H

#ifdef __cplusplus
extern "C" {
#endif

#ifndef ZERO_INTERFERENCE
void file_log_write(const char *line);
#else
#define file_log_write(line) ((void)0)
#endif

#ifdef __cplusplus
};
#endif

#endif
