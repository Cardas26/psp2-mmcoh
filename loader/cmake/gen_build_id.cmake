set(_id "unknown")
if (EXISTS "${REPO}/.git")
    execute_process(
        COMMAND git -C "${REPO}" describe --tags --always --dirty
        OUTPUT_VARIABLE _id
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
        RESULT_VARIABLE _rc)
    if (NOT _rc EQUAL 0 OR _id STREQUAL "")
        set(_id "unknown")
    endif ()
endif ()

set(_content "#ifndef SOLOADER_BUILD_ID_GENERATED_H
#define SOLOADER_BUILD_ID_GENERATED_H
#define BUILD_ID_GIT      \"${_id}\"
#define BUILD_ID_PROFILE  \"${PROFILE}\"
#define BUILD_ID_VGLFLAGS \"${VGLFLAGS}\"
#define BUILD_ID_OPTIONS  \"${PROFILE}\"
#endif
")

set(_stale TRUE)
if (EXISTS "${OUT}")
    file(READ "${OUT}" _existing)
    if (_existing STREQUAL _content)
        set(_stale FALSE)
    endif ()
endif ()
if (_stale)
    file(WRITE "${OUT}" "${_content}")
endif ()
