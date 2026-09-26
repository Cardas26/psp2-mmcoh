#include "utils/fast_crc32.h"

#include <so_util/so_util.h>
#include <stdint.h>
#include <zlib.h>

#include "utils/logger.h"

extern so_module so_mod;

static uint32_t fast_crc32(const char *buf, uint32_t len) {
    return (uint32_t)crc32(0L, (const Bytef *)buf, len);
}

void fast_crc32_install(void) {
    uintptr_t addr = so_symbol(&so_mod, "_ZN5moFlo10CHashCRC3216GenerateHashCodeEPKcj");
    if (!addr) {
        l_warn("[fast-crc32] CHashCRC32::GenerateHashCode(const char*, unsigned) not found - not installed");
        return;
    }
    hook_addr(addr, (uintptr_t)fast_crc32);
    l_perf("[fast-crc32] installed");
}
