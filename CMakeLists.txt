cmake_minimum_required(VERSION 3.16)

project(psDoom C)

set(PSD_APP_VERSION "0.1.1")
set(PSD_BUILD_ID "${PSD_APP_VERSION}-label-centering-v3")

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
#   tools/fetch-freedoom.sh              # once: the bundled Freedoom IWAD
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
    "${CMAKE_CURRENT_SOURCE_DIR}/src/psdoom/psdoom_lifecycle.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/psdoom/proc_select.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/psdoom/psdoom_options.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/psdoom/psdoom_menu.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/platform/proc_backend.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/platform/proc_script.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/platform/macos/proc_macos.c")
target_compile_definitions(doom PRIVATE PSD_BUILD_ID=\"${PSD_BUILD_ID}\")
target_include_directories(doom PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src/psdoom"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/platform"
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
add_executable(psDoom MACOSX_BUNDLE
    src/app/macos/launcher_main.c
    src/app/macos/iwad_select.c)

set_target_properties(psDoom PROPERTIES
    OUTPUT_NAME "psDoom"
    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/src/app/macos/Info.plist.in"
    MACOSX_BUNDLE_BUNDLE_NAME "psDoom"
    MACOSX_BUNDLE_GUI_IDENTIFIER "com.resourcefork.psdoom"
    MACOSX_BUNDLE_BUNDLE_VERSION "${PSD_APP_VERSION}"
    MACOSX_BUNDLE_SHORT_VERSION_STRING "${PSD_APP_VERSION}"
)

#
# Bundled default IWAD: Freedoom: Phase 1 (BSD-3-Clause). tools/fetch-freedoom.sh
# downloads the pinned release into the git-ignored wads/ folder; the WAD is
# never committed. The build verifies it against tools/freedoom.sha256, ships it
# with its licence and credits in Contents/Resources, and validates the arena
# against it. Configure with -DPSDOOM_BUNDLE_FREEDOOM=OFF to build without it
# (the app then needs a registered DOOM.WAD from the user to start).
#
option(PSDOOM_BUNDLE_FREEDOOM "Bundle wads/freedoom1.wad as the default IWAD" ON)
set(_PSD_FREEDOOM_WAD     "${CMAKE_CURRENT_SOURCE_DIR}/wads/freedoom1.wad")
set(_PSD_FREEDOOM_COPYING "${CMAKE_CURRENT_SOURCE_DIR}/wads/freedoom-COPYING.txt")
set(_PSD_FREEDOOM_CREDITS "${CMAKE_CURRENT_SOURCE_DIR}/wads/freedoom-CREDITS.txt")
set(_PSD_FREEDOOM_MUSIC   "${CMAKE_CURRENT_SOURCE_DIR}/wads/freedoom-CREDITS-MUSIC.txt")
set(_PSD_FREEDOOM_PINS    "${CMAKE_CURRENT_SOURCE_DIR}/tools/freedoom.sha256")

if(PSDOOM_BUNDLE_FREEDOOM)
    foreach(_f "${_PSD_FREEDOOM_WAD}" "${_PSD_FREEDOOM_COPYING}"
            "${_PSD_FREEDOOM_CREDITS}" "${_PSD_FREEDOOM_MUSIC}")
        if(NOT EXISTS "${_f}")
            message(FATAL_ERROR
                "psDoom: ${_f} is missing. Run tools/fetch-freedoom.sh to download "
                "the bundled Freedoom IWAD, then configure again (or configure "
                "with -DPSDOOM_BUNDLE_FREEDOOM=OFF to build without it).")
        endif()
    endforeach()
    file(STRINGS "${_PSD_FREEDOOM_PINS}" _psd_pin REGEX "  freedoom1\\.wad$")
    string(REGEX MATCH "^[0-9a-f]+" _psd_pin "${_psd_pin}")
    file(SHA256 "${_PSD_FREEDOOM_WAD}" _psd_have)
    if(NOT _psd_have STREQUAL _psd_pin)
        message(FATAL_ERROR
            "psDoom: ${_PSD_FREEDOOM_WAD} has sha256 ${_psd_have}, but "
            "tools/freedoom.sha256 pins ${_psd_pin}. Delete it and run "
            "tools/fetch-freedoom.sh again.")
    endif()
    # Re-run this check whenever the WAD or the pin changes.
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${_PSD_FREEDOOM_WAD}" "${_PSD_FREEDOOM_PINS}")
    message(STATUS "psDoom: bundling verified wads/freedoom1.wad (sha256 ${_psd_have})")
else()
    message(WARNING
        "psDoom: PSDOOM_BUNDLE_FREEDOOM is OFF. The app bundles no IWAD and will "
        "not start unless the user supplies a registered DOOM.WAD.")
endif()

# Generate and independently validate the arena in the build tree. With the
# bundled Freedoom present its resources are validated too; builds without it
# still receive full structural PWAD validation.
find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter)
set(_PSD_ARENA_WAD "${CMAKE_CURRENT_BINARY_DIR}/generated/psdoom-arena.wad")
set(_PSD_ARENA_WAD_TMP "${CMAKE_CURRENT_BINARY_DIR}/generated/psdoom-arena.wad.tmp")
set(_PSD_ARENA_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/tools/generate_octagonal_arena.py"
    "${CMAKE_CURRENT_SOURCE_DIR}/tools/validate_wad.py")
set(_PSD_ARENA_VALIDATE_ARGS --wad "${_PSD_ARENA_WAD_TMP}")
if(PSDOOM_BUNDLE_FREEDOOM)
    list(APPEND _PSD_ARENA_VALIDATE_ARGS --iwad "${_PSD_FREEDOOM_WAD}")
    list(APPEND _PSD_ARENA_DEPENDS "${_PSD_FREEDOOM_WAD}")
endif()
# Freedoom's texture set includes Doom II names, and a registered DOOM.WAD
# takes precedence at runtime, so also check the arena's resources against an
# id Doom IWAD when the developer has one in wads/ (only read, never bundled).
set(_PSD_ARENA_ID_VALIDATE)
foreach(_psd_id_iwad DOOM.WAD DOOM1.WAD)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/wads/${_psd_id_iwad}")
        list(APPEND _PSD_ARENA_ID_VALIDATE
            COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_CURRENT_SOURCE_DIR}/tools/validate_wad.py"
                --wad "${_PSD_ARENA_WAD_TMP}"
                --iwad "${CMAKE_CURRENT_SOURCE_DIR}/wads/${_psd_id_iwad}")
        list(APPEND _PSD_ARENA_DEPENDS
            "${CMAKE_CURRENT_SOURCE_DIR}/wads/${_psd_id_iwad}")
        message(STATUS "psDoom: also validating the arena against wads/${_psd_id_iwad}")
        break()
    endif()
endforeach()

add_custom_command(
    OUTPUT "${_PSD_ARENA_WAD}"
    COMMAND ${CMAKE_COMMAND} -E make_directory
        "${CMAKE_CURRENT_BINARY_DIR}/generated"
    COMMAND ${CMAKE_COMMAND} -E rm -f "${_PSD_ARENA_WAD_TMP}"
    COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_CURRENT_SOURCE_DIR}/tools/generate_octagonal_arena.py"
        --output "${_PSD_ARENA_WAD_TMP}"
    COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_CURRENT_SOURCE_DIR}/tools/validate_wad.py"
        ${_PSD_ARENA_VALIDATE_ARGS}
    ${_PSD_ARENA_ID_VALIDATE}
    COMMAND ${CMAKE_COMMAND} -E rename
        "${_PSD_ARENA_WAD_TMP}" "${_PSD_ARENA_WAD}"
    DEPENDS ${_PSD_ARENA_DEPENDS}
    COMMENT "Generating and validating the psDoom octagonal arena"
    VERBATIM
)
add_custom_target(psdoom_arena_wad DEPENDS "${_PSD_ARENA_WAD}")

add_dependencies(psDoom crispy-doom psdoom_arena_wad)

# Assemble the bundle as a POST_BUILD step of the app target itself, so building
# (or running) the psDoom target/scheme always yields a complete .app. The
# earlier separate "embed" target wasn't built by the psDoom scheme, leaving the
# engine out of the bundle in Xcode. LINK_DEPENDS makes the app relink -- and so
# re-run this assembly -- whenever either the engine binary or generated arena
# changes. This file dependency prevents incremental builds from leaving a stale
# bundled PWAD (CMake's Xcode post-build phases already run on every build).
set(_PSD_LINK_DEPENDS "$<TARGET_FILE:crispy-doom>;${_PSD_ARENA_WAD}")
if(PSDOOM_BUNDLE_FREEDOOM)
    string(APPEND _PSD_LINK_DEPENDS
        ";${_PSD_FREEDOOM_WAD};${_PSD_FREEDOOM_COPYING};${_PSD_FREEDOOM_CREDITS};${_PSD_FREEDOOM_MUSIC}")
endif()
set_target_properties(psDoom PROPERTIES LINK_DEPENDS "${_PSD_LINK_DEPENDS}")

set(_PSD_MACOS_DIR "$<TARGET_BUNDLE_CONTENT_DIR:psDoom>/MacOS")
set(_PSD_RES_DIR   "$<TARGET_BUNDLE_CONTENT_DIR:psDoom>/Resources")

add_custom_command(TARGET psDoom POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${_PSD_RES_DIR}"
    COMMAND ${CMAKE_COMMAND} -E copy
        "$<TARGET_FILE:crispy-doom>" "${_PSD_MACOS_DIR}/crispy-doom"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${_PSD_ARENA_WAD}" "${_PSD_RES_DIR}/psdoom-arena.wad"
    VERBATIM
)

# The bundled default IWAD and the notices its BSD-3 licence requires. A
# shareware DOOM1.WAD in wads/ is no longer bundled: psDoom never selects it,
# and a copy left in an older build tree is removed. With bundling OFF, any
# Freedoom files an earlier build left in the bundle are removed too.
add_custom_command(TARGET psDoom POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E rm -f "${_PSD_RES_DIR}/DOOM1.WAD"
    VERBATIM
)
if(NOT PSDOOM_BUNDLE_FREEDOOM)
    add_custom_command(TARGET psDoom POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E rm -f
            "${_PSD_RES_DIR}/freedoom1.wad"
            "${_PSD_RES_DIR}/Freedoom-COPYING.txt"
            "${_PSD_RES_DIR}/Freedoom-CREDITS.txt"
            "${_PSD_RES_DIR}/Freedoom-CREDITS-MUSIC.txt"
        VERBATIM
    )
endif()
if(PSDOOM_BUNDLE_FREEDOOM)
    add_custom_command(TARGET psDoom POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${_PSD_FREEDOOM_WAD}" "${_PSD_RES_DIR}/freedoom1.wad"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${_PSD_FREEDOOM_COPYING}" "${_PSD_RES_DIR}/Freedoom-COPYING.txt"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${_PSD_FREEDOOM_CREDITS}" "${_PSD_RES_DIR}/Freedoom-CREDITS.txt"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${_PSD_FREEDOOM_MUSIC}" "${_PSD_RES_DIR}/Freedoom-CREDITS-MUSIC.txt"
        VERBATIM
    )
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

#
# Host tests for engine-free policy layers and SDL keyboard predicates.
# Build and run explicitly with:
#
#   cmake --build build --target psdoom_tests psdoom_window_state_tests
#   ctest --test-dir build --output-on-failure
#
enable_testing()

add_executable(psdoom_tests EXCLUDE_FROM_ALL
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/proc_select_test.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/iwad_select_test.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/app/macos/iwad_select.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/psdoom/proc_select.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/psdoom/psdoom_lifecycle.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/platform/proc_backend.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/platform/proc_script.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/platform/proc_fake.c")
target_compile_definitions(psdoom_tests PRIVATE PSD_NO_NATIVE_BACKEND)
target_include_directories(psdoom_tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src/psdoom"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/platform"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/app/macos")
if(APPLE)
    # Linked for read-only listing/lookup checks and the renice pid <= 1 check;
    # the tests never call the native kill, and the native renice reaches only
    # the test binary's own getpriority/setpriority seam, never the OS.
    target_sources(psdoom_tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/platform/macos/proc_macos.c")
    target_include_directories(psdoom_tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/platform/macos")
endif()

add_test(NAME psdoom_tests COMMAND psdoom_tests)

# Game code must act on processes only through the pid-reuse-checked wrappers.
add_test(NAME psdoom_no_raw_backend_actions
    COMMAND ${CMAKE_COMMAND} -DPSD_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
            -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/no_raw_backend_actions.cmake")

if(NOT DEFINED EMSCRIPTEN)
    add_executable(psdoom_window_state_tests EXCLUDE_FROM_ALL
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/window_state_test.c")
    target_include_directories(psdoom_window_state_tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/third_party/crispy-doom/src")
    target_link_libraries(psdoom_window_state_tests PRIVATE CrispyDoom::SDL2)

    add_test(NAME psdoom_window_state_tests COMMAND psdoom_window_state_tests)
endif()
