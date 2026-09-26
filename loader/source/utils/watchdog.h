#ifndef SOLOADER_WATCHDOG_H
#define SOLOADER_WATCHDOG_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifdef HANG_WATCHDOG

void watchdog_start(void);

void watchdog_note_log_activity(void);

void watchdog_note_wait_begin(uint32_t obj_addr, uint32_t self_tid, uint32_t owner_tid);
void watchdog_note_wait_end(void);

#else

#define watchdog_start()                                    ((void)0)
#define watchdog_note_log_activity()                        ((void)0)
#define watchdog_note_wait_begin(obj_addr, self_tid, owner) \
    do { (void)(obj_addr); (void)(self_tid); (void)(owner); } while (0)
#define watchdog_note_wait_end()                            ((void)0)

#endif

#ifdef __cplusplus
};
#endif

#endif
