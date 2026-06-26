cmake_minimum_required(VERSION 3.16)

project(psDoom C)

# Generate a shared Xcode scheme only for the psDoom app (must be set before the
# targets are created). Keeps Xcode's "Run" pointed at the bundle (which locates
# the WAD) instead of the bare engine binaries.
set(CMAKE_XCODE_GENERATE_SCHEME ON)

#
# psDoom is built on a vendored Crispy Doom engine (see third_party/README.md)
# and wraps it in a double-clickable macOS .app bundle.
#
# Repository layout:
#   third_party/crispy-doom/  vendored engine, kept pristine (built here)
#   src/app/macos/            the .app launcher (this bundle's executable)
#   src/psdoom/               process<->monster game logic        (added later)
#   src/platform/macos/       native libproc/renice/kill backend  (added later)
#
# Build (command line):
#   cmake -S . -B build
#   cmake --build build -j
#   open build/psDoom.app
#
# Build (Xcode project, generated from this same CMake):
#   cmake -G Xcode -S . -B build-xcode
#   open build-xcode/psDoom.xcodeproj    # select the "psDoom" scheme, then Run
#

# psDoom has no use for netplay; keep the dependency surface small.
set(ENABLE_SDL2_NET OFF CACHE BOOL "Disable SDL2_net (psDoom has no netplay)" FORCE)

# Build the vendored engine. Defines the `crispy-doom` executable target.
add_subdirectory(third_party/crispy-doom)

# Compile the psDoom module into the engine's `doom` static library, and let the
# engine call-sites find <psdoom.h>. Done from here so the vendored CMake under
# third_party/ stays untouched.
target_sources(doom PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src/psdoom/psdoom.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/platform/macos/proc_macos.c")
target_include_directories(doom PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src/psdoom"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/platform/macos"
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/crispy-doom/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/crispy-doom/src/doom")

if(NOT APPLE)
    message(FATAL_ERROR
        "The psDoom .app bundle target is macOS-only. On other platforms build "
        "the engine directly under third_party/crispy-doom.")
endif()

#
# The .app's main executable is a tiny compiled launcher (a real Mach-O, so the
# bundle can be code-signed). It locates an IWAD and exec()s the engine binary
# that sits beside it in Contents/MacOS.
#
add_executable(psDoom MACOSX_BUNDLE src/app/macos/launcher_main.c)

set_target_properties(psDoom PROPERTIES
    OUTPUT_NAME "psDoom"
    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/src/app/macos/Info.plist.in"
    MACOSX_BUNDLE_BUNDLE_NAME "psDoom"
    MACOSX_BUNDLE_GUI_IDENTIFIER "com.resourcefork.psdoom"
    MACOSX_BUNDLE_BUNDLE_VERSION "0.1.0"
    MACOSX_BUNDLE_SHORT_VERSION_STRING "0.1.0"
)

add_dependencies(psDoom crispy-doom)

# Assemble the bundle on EVERY build via an always-out-of-date custom target.
# A plain POST_BUILD on psDoom is skipped when only the engine changed (the
# launcher doesn't relink), which would leave a stale crispy-doom inside the
# .app. This target re-copies whenever either psDoom or the engine is rebuilt.
set(_PSD_MACOS_DIR "$<TARGET_BUNDLE_CONTENT_DIR:psDoom>/MacOS")
set(_PSD_RES_DIR   "$<TARGET_BUNDLE_CONTENT_DIR:psDoom>/Resources")

add_custom_target(psdoom_embed ALL
    COMMAND ${CMAKE_COMMAND} -E make_directory "${_PSD_RES_DIR}"
    COMMAND ${CMAKE_COMMAND} -E copy
        "$<TARGET_FILE:crispy-doom>" "${_PSD_MACOS_DIR}/crispy-doom"
    VERBATIM
)
add_dependencies(psdoom_embed psDoom crispy-doom)

# For local double-click play, bundle an IWAD if one is present in wads/.
# Build-time convenience only; the WAD is gitignored and never committed.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/wads/DOOM1.WAD")
    add_custom_command(TARGET psdoom_embed POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy
            "${CMAKE_CURRENT_SOURCE_DIR}/wads/DOOM1.WAD"
            "${_PSD_RES_DIR}/DOOM1.WAD"
        VERBATIM
    )
    message(STATUS "psDoom: will bundle wads/DOOM1.WAD into the .app")
else()
    message(STATUS "psDoom: no wads/DOOM1.WAD found; the app will search "
        "DOOMWADPATH / ~/Library/Application Support/psDoom at runtime")
endif()

# Expose only the psDoom app as a runnable Xcode scheme. Running the bare engine
# targets bypasses the launcher (and the bundled WAD), which only causes
# confusion, so suppress their schemes.
set_target_properties(psDoom PROPERTIES XCODE_GENERATE_SCHEME ON)
foreach(_eng
        crispy-doom crispy-heretic crispy-hexen crispy-strife crispy-setup
        crispy-server doom heretic hexen strife setup textscreen opl pcsound
        mus2mid midiread)
    if(TARGET ${_eng})
        set_target_properties(${_eng} PROPERTIES XCODE_GENERATE_SCHEME OFF)
    endif()
endforeach()
