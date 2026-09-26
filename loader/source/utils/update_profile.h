#ifndef SOLOADER_UPDATE_PROFILE_H
#define SOLOADER_UPDATE_PROFILE_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint64_t t0;
    int      depth;
    int      slot;
    bool     on;
} uprof_hspan_t;
#define UPROF_HSPAN_PALETTE  0
#define UPROF_HSPAN_ANIM_CTL 1

#define uprof_on_update_thread()                 (false)

#endif
