# psDoom -- SetVideoMode turns SDL text input off once the window exists.
#
# SDL starts with text input on, and while it is on macOS shows its
# press-and-hold accent popup instead of repeating a held key. The fix is the
# I_SyncTextInput() call at the end of SetVideoMode in Crispy's i_video.c;
# text_input_test.c checks what I_SyncTextInput does, and this checks that
# SetVideoMode still calls it.
#
# Usage: cmake -DPSD_SOURCE_DIR=<repo> -P tests/text_input_sync_call.cmake

if(NOT PSD_SOURCE_DIR)
    message(FATAL_ERROR "PSD_SOURCE_DIR is not set")
endif()

set(_file "${PSD_SOURCE_DIR}/third_party/crispy-doom/src/i_video.c")
file(READ "${_file}" _src)

# The body of SetVideoMode: from its definition to the first closing brace
# at the start of a line.
string(FIND "${_src}" "\nstatic void SetVideoMode(void)\n{" _start)
if(_start EQUAL -1)
    message(FATAL_ERROR "SetVideoMode not found in ${_file}")
endif()
string(SUBSTRING "${_src}" ${_start} -1 _rest)
string(FIND "${_rest}" "\n}\n" _end)
if(_end EQUAL -1)
    message(FATAL_ERROR "end of SetVideoMode not found in ${_file}")
endif()
string(SUBSTRING "${_rest}" 0 ${_end} _body)

if(NOT _body MATCHES "\n[ \t]*I_SyncTextInput\\(\\);")
    message(FATAL_ERROR
        "SetVideoMode in ${_file} no longer calls I_SyncTextInput(): SDL text "
        "input would stay on in play and macOS would show the accent popup")
endif()

message(STATUS "SetVideoMode calls I_SyncTextInput()")
