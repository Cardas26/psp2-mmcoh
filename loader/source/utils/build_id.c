#include "utils/build_id.h"
#include "utils/logger.h"

#include "build_id_generated.h"

__attribute__((used))
const char g_build_id[] =
    "MMCOH-BUILD " BUILD_ID_GIT " [" BUILD_ID_PROFILE "]"
    " vitaGL(" BUILD_ID_VGLFLAGS ")";

__attribute__((used))
const char g_build_options[] = "MMCOH-OPTIONS " BUILD_ID_OPTIONS;

void build_id_log(void) {
    l_fatal("%s", g_build_id);
    l_fatal("%s", g_build_options);
}
