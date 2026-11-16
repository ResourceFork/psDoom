# psDoom -- static check: the player never sees the protocol's words.
#
# The effect setting reads Off / Wounds only / Wounds and kills, and its help
# line uses the plugin's plain words. "dry run", "post", "delete" and "renice"
# stay in the protocol, the CLI and the spec. This scans the string literals of
# the files that draw the menu and its help (any case), and every game source
# for the old setting names. A tripwire, not a proof.
#
# Run by ctest as:  cmake -DPSD_SOURCE_DIR=<repo> -P no_effect_jargon.cmake

if(NOT PSD_SOURCE_DIR)
    message(FATAL_ERROR "PSD_SOURCE_DIR is not set")
endif()

set(_psd_shown
    "${PSD_SOURCE_DIR}/src/psdoom/psdoom_menu.c"
    "${PSD_SOURCE_DIR}/src/psdoom/psdoom_effect.c")
file(GLOB_RECURSE _psd_all
    "${PSD_SOURCE_DIR}/src/psdoom/*.c"
    "${PSD_SOURCE_DIR}/src/psdoom/*.h")

set(_psd_bad "")
# Any string literal in the drawn files naming a protocol word, in any case.
set(_psd_word
    "\"[^\"]*([Dd][Rr][Yy][ -]?[Rr][Uu][Nn]|[Pp][Oo][Ss][Tt]|[Dd][Ee][Ll][Ee][Tt][Ee]|[Rr][Ee][Nn][Ii][Cc][Ee]|SIGTERM)[^\"]*\"")
foreach(_f IN LISTS _psd_shown)
    if(NOT EXISTS "${_f}")
        message(FATAL_ERROR "missing ${_f}")
    endif()
    file(STRINGS "${_f}" _hits REGEX "${_psd_word}")
    foreach(_h IN LISTS _hits)
        list(APPEND _psd_bad "${_f}: ${_h}")
    endforeach()
endforeach()
# The old setting names, anywhere in the game.
foreach(_f IN LISTS _psd_all)
    file(STRINGS "${_f}" _hits
         REGEX "\"[^\"]*(dry run\\)|post only|post \\+ delete|\\(KILL\\))[^\"]*\"")
    foreach(_h IN LISTS _hits)
        list(APPEND _psd_bad "${_f}: ${_h}")
    endforeach()
endforeach()

if(_psd_bad)
    list(JOIN _psd_bad "\n" _psd_report)
    message(FATAL_ERROR "protocol words in text the player sees:\n${_psd_report}")
endif()

list(LENGTH _psd_all _psd_n)
message(STATUS "no protocol words in the menu text (${_psd_n} game files scanned)")
