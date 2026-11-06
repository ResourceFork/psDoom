# psDoom -- static check: game code acts on processes only through the
# pid-reuse-checked wrappers (psd_proc_renice_checked / psd_proc_kill_checked),
# never through the raw backend renice/kill ops, which act by pid alone.
#
# Run by ctest as:  cmake -DPSD_SOURCE_DIR=<repo> -P no_raw_backend_actions.cmake

if(NOT PSD_SOURCE_DIR)
    message(FATAL_ERROR "PSD_SOURCE_DIR is not set")
endif()

file(GLOB_RECURSE _psd_game_sources
    "${PSD_SOURCE_DIR}/src/psdoom/*.c"
    "${PSD_SOURCE_DIR}/src/psdoom/*.h"
    "${PSD_SOURCE_DIR}/src/app/*.c")

if(NOT _psd_game_sources)
    message(FATAL_ERROR "no game sources found under ${PSD_SOURCE_DIR}/src")
endif()

set(_psd_bad "")
foreach(_f IN LISTS _psd_game_sources)
    file(STRINGS "${_f}" _hits REGEX "->[ \t]*(renice|kill)[ \t]*\\(")
    foreach(_h IN LISTS _hits)
        list(APPEND _psd_bad "${_f}: ${_h}")
    endforeach()
endforeach()

if(_psd_bad)
    list(JOIN _psd_bad "\n" _psd_report)
    message(FATAL_ERROR
        "raw backend renice/kill call in game code (use psd_proc_*_checked):\n"
        "${_psd_report}")
endif()

list(LENGTH _psd_game_sources _psd_n)
message(STATUS "no raw backend renice/kill calls in ${_psd_n} game source files")
