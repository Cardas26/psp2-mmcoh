cmake_minimum_required(VERSION 3.14)

if (NOT DEFINED ENV{VITASDK} AND NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  message(FATAL_ERROR "Please define VITASDK env var to point to your SDK path or set CMAKE_TOOLCHAIN_FILE.")
endif ()

set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "Toolchain file")
if (NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif ()

project(so_loader C CXX)
include("${VITASDK}/share/vita.cmake" REQUIRED)

set(VITA_APP_NAME "Clash of Heroes")
set(VITA_TITLEID "MMCOH0001")
set(VITA_VPKNAME "mmcoh")
set(VITA_VERSION "01.00")
set(VITA_MKSFOEX_FLAGS "-d ATTRIBUTE2=12")

option(DIAGNOSTICS "Build with debug.log, the hang watchdog and the fps counter" OFF)

add_definitions(
  -DAPK_PATH="ux0:data/mmcoh/ClashOfHeroes.apk"
  -DAUDIO_PUMP_AHEAD=2
  -DAUDIO_PUMP_PRIORITY=0x40
  -DDATA_PATH="ux0:data/mmcoh/"
  -DFMODEVENT_PATH="ux0:data/mmcoh/libfmodevent.so"
  -DFMODEX_PATH="ux0:data/mmcoh/libfmodex.so"
  -DGPU_CLOCK_MHZ=166
  -DLOADING_SCREEN_DRAIN_MS=8
  -DPKG_NAME="com.ubisoft.mightandmagic.clashofheroes"
  -DSO_PATH="ux0:data/mmcoh/libApplication.so"
  -DTEXTURE_BUDGET_MB=160
  -D__POSIX_VISIBLE=999999
  -D_GNU_SOURCE
  -D__POSIX_VISIBLE=999999)

set(VITAGL_MAKE_TAIL "HAVE_WVP_ON_GPU=1 HAVE_SHADER_CACHE=1 NO_SPLASHSCREEN=1")
if (DIAGNOSTICS)
  add_definitions(-DDEBUG_SOLOADER -DHANG_WATCHDOG -DPERF_COUNTER_ENABLED -DFALSOJNI_DEBUGLEVEL=2)
  set(VITAGL_MAKE_FLAGS "SOFTFP_ABI=1 LOG_ERRORS=2 ${VITAGL_MAKE_TAIL}")
  set(BUILD_PROFILE "diagnostics")
else ()
  add_definitions(-DZERO_INTERFERENCE -DGAME_LOG_QUIET -DFALSOJNI_DEBUGLEVEL=4)
  set(VITAGL_MAKE_FLAGS "SOFTFP_ABI=1 NO_DEBUG=1 ${VITAGL_MAKE_TAIL}")
  set(BUILD_PROFILE "zero-interference")
endif ()
add_definitions(-DVITAGL_FLAGS_STR="${VITAGL_MAKE_FLAGS}")
message(STATUS "Build profile: ${BUILD_PROFILE}")

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wl,-q -mfloat-abi=softfp -std=gnu11 -Wno-deprecated")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wl,-q -mfloat-abi=softfp -std=gnu++20 -Wno-write-strings -Wno-psabi")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -ffunction-sections -fdata-sections -O3 -g0 -ffast-math")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -ffunction-sections -fdata-sections -O3 -g0 -ffast-math")
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -Wl,--gc-sections")

set(PORT_SOURCES
    source/main.c
    source/dynlib.c
    source/java.c
    source/patch.c
    source/video.c
    source/subtitles.c
    source/reimpl/bits/_ctype.c
    source/reimpl/errno.c
    source/reimpl/io.c
    source/reimpl/iocache.c
    source/reimpl/iocontent.c
    source/reimpl/iopath.c
    source/reimpl/log.c
    source/reimpl/mem.c
    source/reimpl/pthr.c
    source/reimpl/sys.c
    source/utils/build_id.c
    source/utils/dialog.c
    source/utils/filelog.c
    source/utils/first_boot.c
    source/utils/glutil.c
    source/utils/init.c
    source/utils/logger.c
    source/utils/setup_data.c
    source/utils/render_command_cache.c
    source/utils/render_find.c
    source/utils/render_material.c
    source/utils/render_rbuf.c
    source/utils/render_sprdata.c
    source/utils/render_sortprep.c
    source/utils/render_dynrend.c
    source/utils/sprite_setter_c.c
    source/utils/ai_board_copy.c
    source/utils/ai_core_memo.c
    source/utils/cheatsheet_lazy.c
    source/utils/palette_info_memo.c
    source/utils/sprite_dir_record.c
    source/utils/scene_entity_index.c
    source/utils/sprite_state_cache.c
    source/utils/transform_cheap_push.c
    source/utils/bmptext_cache.c
    source/utils/cheatsheet_grid_gate.c
    source/utils/transform_same_gate.c
    source/utils/game_log_quiet.c
    source/utils/event_update_gate.c
    source/utils/text_overlay_cache.c
    source/reimpl/inflate_fast.c
    source/utils/fast_crc32.c
    source/utils/loading_drain.c
    source/audio/music_rewrite.c
    source/audio/music_seg_cache.c
    source/audio/save_names.c
    source/utils/shader_cache.c
    source/utils/softfloat_vfp.c
    source/utils/utils.c
    source/utils/watchdog.c
    source/reimpl/audio.c
    source/reimpl/controls.c
)
set(SOURCES
    source/main.c
    source/dynlib.c
    source/java.c
    source/patch.c
    source/video.c
    source/subtitles.c
    source/reimpl/bits/_ctype.c
    source/reimpl/errno.c
    source/reimpl/io.c
    source/reimpl/iocache.c
    source/reimpl/iocontent.c
    source/reimpl/iopath.c
    source/reimpl/log.c
    source/reimpl/mem.c
    source/reimpl/pthr.c
    source/reimpl/sys.c
    source/utils/build_id.c
    source/utils/dialog.c
    source/utils/filelog.c
    source/utils/first_boot.c
    source/utils/glutil.c
    source/utils/init.c
    source/utils/logger.c
    source/utils/setup_data.c
    source/utils/render_command_cache.c
    source/utils/render_find.c
    source/utils/render_material.c
    source/utils/render_rbuf.c
    source/utils/render_sprdata.c
    source/utils/render_sortprep.c
    source/utils/render_dynrend.c
    source/utils/sprite_setter_c.c
    source/utils/ai_board_copy.c
    source/utils/ai_core_memo.c
    source/utils/cheatsheet_lazy.c
    source/utils/palette_info_memo.c
    source/utils/sprite_dir_record.c
    source/utils/scene_entity_index.c
    source/utils/sprite_state_cache.c
    source/utils/transform_cheap_push.c
    source/utils/bmptext_cache.c
    source/utils/cheatsheet_grid_gate.c
    source/utils/transform_same_gate.c
    source/utils/game_log_quiet.c
    source/utils/event_update_gate.c
    source/utils/text_overlay_cache.c
    source/reimpl/inflate_fast.c
    source/utils/fast_crc32.c
    source/utils/loading_drain.c
    source/audio/music_rewrite.c
    source/audio/music_seg_cache.c
    source/audio/save_names.c
    source/utils/shader_cache.c
    source/utils/softfloat_vfp.c
    source/utils/utils.c
    source/utils/watchdog.c
    lib/falso_jni/FalsoJNI.c
    lib/falso_jni/FalsoJNI_ImplBridge.c
    lib/falso_jni/FalsoJNI_Logger.c
    lib/falso_jni/converter.c
    lib/sha1/sha1.c
    lib/fios/fios.c
    lib/so_util/so_util.c
    source/reimpl/audio.c
    source/reimpl/controls.c
)

set(BUILD_ID_HEADER_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated)
file(MAKE_DIRECTORY ${BUILD_ID_HEADER_DIR})
add_custom_target(build_id_header ALL
    COMMAND ${CMAKE_COMMAND}
            -DOUT=${BUILD_ID_HEADER_DIR}/build_id_generated.h
            -DREPO=${CMAKE_CURRENT_SOURCE_DIR}/..
            -DPROFILE=${BUILD_PROFILE}
            "-DVGLFLAGS=${VITAGL_MAKE_FLAGS}"
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/gen_build_id.cmake
    BYPRODUCTS ${BUILD_ID_HEADER_DIR}/build_id_generated.h)

add_executable(${CMAKE_PROJECT_NAME} ${SOURCES})
set_target_properties(${CMAKE_PROJECT_NAME} PROPERTIES LINKER_LANGUAGE CXX)
add_dependencies(${CMAKE_PROJECT_NAME} build_id_header)
target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE ${BUILD_ID_HEADER_DIR})
set_source_files_properties(${PORT_SOURCES}
    PROPERTIES COMPILE_OPTIONS "-Wformat;-Wformat-security")
set_source_files_properties(source/dynlib.c
    PROPERTIES COMPILE_OPTIONS "-Wformat;-Wformat-security;-Wno-deprecated-declarations")

set(ZLIB_COMPAT               ON  CACHE BOOL "" FORCE)
set(ZLIB_ENABLE_TESTS         OFF CACHE BOOL "" FORCE)
set(ZLIBNG_ENABLE_TESTS       OFF CACHE BOOL "" FORCE)
set(WITH_GTEST                OFF CACHE BOOL "" FORCE)
set(WITH_GZFILEOP             ON  CACHE BOOL "" FORCE)
set(WITH_RUNTIME_CPU_DETECTION OFF CACHE BOOL "" FORCE)
set(WITH_NEON                 ON  CACHE BOOL "" FORCE)
set(WITH_ACLE                 ON  CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS         OFF CACHE BOOL "" FORCE)
add_subdirectory(lib/zlib-ng EXCLUDE_FROM_ALL)
target_include_directories(${CMAKE_PROJECT_NAME} BEFORE PRIVATE
    ${CMAKE_CURRENT_BINARY_DIR}/lib/zlib-ng
    ${CMAKE_CURRENT_SOURCE_DIR}/lib/zlib-ng)

set(LIBDEFLATE_BUILD_SHARED_LIB    OFF CACHE BOOL "" FORCE)
set(LIBDEFLATE_BUILD_GZIP          OFF CACHE BOOL "" FORCE)
set(LIBDEFLATE_BUILD_TESTS         OFF CACHE BOOL "" FORCE)
set(LIBDEFLATE_COMPRESSION_SUPPORT OFF CACHE BOOL "" FORCE)
add_subdirectory(lib/libdeflate EXCLUDE_FROM_ALL)
target_link_libraries(${CMAKE_PROJECT_NAME} libdeflate_static)

add_subdirectory(lib/libc_bridge)
add_dependencies(${CMAKE_PROJECT_NAME} SceLibcBridge)

separate_arguments(VITAGL_MAKE_ARGS UNIX_COMMAND "${VITAGL_MAKE_FLAGS}")
set(VITAGL_DIR ${CMAKE_SOURCE_DIR}/lib/vitagl)
set(VITAGL_LIB ${VITAGL_DIR}/libvitaGL.a)
include(ProcessorCount)
ProcessorCount(VITAGL_NPROC)
if (VITAGL_NPROC EQUAL 0)
  set(VITAGL_NPROC 4)
endif ()
add_custom_target(vitaGL_build
                  COMMAND ${CMAKE_SOURCE_DIR}/extras/scripts/build_vitagl.sh ${VITAGL_DIR} ${VITAGL_NPROC} ${VITAGL_MAKE_ARGS}
                  BYPRODUCTS ${VITAGL_LIB}
                  COMMENT "Building vitaGL (${VITAGL_MAKE_FLAGS})"
                  VERBATIM)
add_library(vitaGL STATIC IMPORTED)
set_target_properties(vitaGL PROPERTIES
                      IMPORTED_LOCATION ${VITAGL_LIB}
                      INTERFACE_INCLUDE_DIRECTORIES ${VITAGL_DIR}/source)
add_dependencies(${CMAKE_PROJECT_NAME} vitaGL_build)

target_include_directories(${CMAKE_PROJECT_NAME} SYSTEM PUBLIC ${CMAKE_SOURCE_DIR}/lib)
target_include_directories(${CMAKE_PROJECT_NAME} PUBLIC ${CMAKE_SOURCE_DIR}/source)

target_link_libraries(${CMAKE_PROJECT_NAME}
                      -Wl,--whole-archive pthread -Wl,--no-whole-archive
                      OpenSLES
                      sndfile
                      mp3lame
                      mpg123
                      opus
                      vorbis
                      ogg
                      vorbisenc
                      vorbisfile
                      FLAC
                      stdc++
                      vitaGL
                      vitashark
                      zlib
                      SceShaccCgExt
                      taihen_stub
                      SceShaccCg_stub
                      z
                      m
                      ScePgf_stub
                      mathneon
                      kubridge_stub
                      ${CMAKE_BINARY_DIR}/lib/libc_bridge/stubs/libSceLibcBridge_stub.a
                      SceHid_stub
                      SceTouch_stub
                      SceMotion_stub
                      SceAppMgr_stub
                      SceAppUtil_stub
                      SceAudio_stub
                      SceAudioIn_stub
                      SceSysmodule_stub
                      SceAvPlayer_stub
                      ScePgf_stub
                      SceCommonDialog_stub
                      SceDisplay_stub
                      SceKernelDmacMgr_stub
                      SceFios2_stub
                      SceCtrl_stub
                      SceGxm_stub
                      ScePower_stub
                      SceSysmem_stub
                      SceVshBridge_stub
                      )

vita_create_self(eboot.bin ${CMAKE_PROJECT_NAME} UNSAFE NOASLR)
vita_create_vpk(${VITA_VPKNAME}.vpk ${VITA_TITLEID} eboot.bin
                VERSION ${VITA_VERSION}
                NAME ${VITA_APP_NAME}
                FILE ${CMAKE_SOURCE_DIR}/extras/empty empty
                ${CMAKE_SOURCE_DIR}/extras/cpuinfo cpuinfo
                ${CMAKE_SOURCE_DIR}/extras/meminfo meminfo
                ${CMAKE_SOURCE_DIR}/extras/urandom urandom
                ${CMAKE_SOURCE_DIR}/extras/livearea/icon0.png sce_sys/icon0.png
                ${CMAKE_SOURCE_DIR}/extras/livearea/pic0.png sce_sys/pic0.png
                ${CMAKE_SOURCE_DIR}/extras/livearea/startup.png sce_sys/livearea/contents/startup.png
                ${CMAKE_SOURCE_DIR}/extras/livearea/bg0.png sce_sys/livearea/contents/bg0.png
                ${CMAKE_SOURCE_DIR}/extras/livearea/template.xml sce_sys/livearea/contents/template.xml
                )
