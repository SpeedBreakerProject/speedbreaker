# SpeedBreaker. GPL-3.0-or-later (see COPYING).
#
#   cmake -S . -B build/main -G Ninja -DCMAKE_BUILD_TYPE=Release \
#         -DCMAKE_C_COMPILER=/opt/homebrew/opt/llvm/bin/clang \
#         -DCMAKE_CXX_COMPILER=/opt/homebrew/opt/llvm/bin/clang++
#   ninja -C build/main -j6
#
# Needs ppc/ (XenonRecomp output from your own default.xex) and tools/
# (scripts/setup_tools.sh). -DSB_TESTS=ON adds the unit tests (tests/; run
# them with ctest from the build folder).
cmake_minimum_required(VERSION 3.20)

# macOS (not iOS, which scripts/ios/build.sh sets to 17.0): the oldest macOS
# the game runs on, 15.0 (Sequoia), which the Mac app declares
# (scripts/macos/make_app.py reads it from the binaries and refuses a library
# that needs a newer one). The libraries the app bundles must be built for it
# too: scripts/macos/build_deps.sh builds them from source (Homebrew's
# bottles are built for the macOS they were made on, 26.0 on macOS 26), and
# the build takes them with -DCMAKE_PREFIX_PATH=<its prefix>
# -DFFMPEG_XMA=<its prefix>/ffmpeg-xma. The target sets only which macOS
# APIs and libc++ features the code may use: the CPU code is the same for
# any target (clang's default for arm64 Macs, apple-m1), and
# scripts/macos/check_codegen.py proves it for a release's commit (its game
# code for this target against the same code for macOS 26.0, function by
# function). Before project(): it picks the SDK's settings.
#
# A build folder keeps the target it was first configured with, so a folder
# still holding one of our earlier defaults (26.0, up to v0.1.0) moves to
# this one, with a line saying so. SB_OSX_TARGET_FROM remembers where the
# folder's target came from: "default <version>" (ours, which follows this
# file), or "chosen" (given with -DCMAKE_OSX_DEPLOYMENT_TARGET=..., or
# changed in the cache: kept; scripts/macos/make_app.py warns that such an
# app isn't the release's).
set(SB_OSX_DEFAULT_TARGET "15.0")
set(SB_OSX_EARLIER_DEFAULTS "26.0")  # in folders configured before SB_OSX_TARGET_FROM
if(CMAKE_HOST_APPLE AND NOT CMAKE_SYSTEM_NAME STREQUAL "iOS")
    set(_sb_follow OFF)
    if(NOT DEFINED CMAKE_OSX_DEPLOYMENT_TARGET)
        set(_sb_follow ON)
    elseif(DEFINED CACHE{SB_OSX_TARGET_FROM})
        # Ours, unless it has been changed since.
        set(_sb_follow OFF)
        if(SB_OSX_TARGET_FROM STREQUAL "default ${CMAKE_OSX_DEPLOYMENT_TARGET}")
            set(_sb_follow ON)
        endif()
    elseif(DEFINED CACHE{SpeedBreaker_BINARY_DIR} AND (CMAKE_OSX_DEPLOYMENT_TARGET IN_LIST SB_OSX_EARLIER_DEFAULTS
           OR CMAKE_OSX_DEPLOYMENT_TARGET STREQUAL SB_OSX_DEFAULT_TARGET))
        # Configured before SB_OSX_TARGET_FROM (project() has run in this
        # folder before, so it isn't a -D of a first configure) with one of
        # our defaults.
        set(_sb_follow ON)
    endif()
    if(_sb_follow)
        if(DEFINED CMAKE_OSX_DEPLOYMENT_TARGET AND NOT CMAKE_OSX_DEPLOYMENT_TARGET STREQUAL SB_OSX_DEFAULT_TARGET)
            message(STATUS "macOS deployment target: ${CMAKE_OSX_DEPLOYMENT_TARGET} (an earlier default) -> ${SB_OSX_DEFAULT_TARGET}")
        endif()
        set(CMAKE_OSX_DEPLOYMENT_TARGET "${SB_OSX_DEFAULT_TARGET}" CACHE STRING "The oldest macOS the game runs on" FORCE)
        set(SB_OSX_TARGET_FROM "default ${SB_OSX_DEFAULT_TARGET}" CACHE INTERNAL "Where CMAKE_OSX_DEPLOYMENT_TARGET came from")
    else()
        set(SB_OSX_TARGET_FROM "chosen" CACHE INTERNAL "Where CMAKE_OSX_DEPLOYMENT_TARGET came from")
    endif()
endif()

# The version: the one place it is set. Everything else comes from it: the
# log's [build] line, Settings > Advanced, bug reports' names, the Mac and iOS
# apps' versions, the SteamOS bundles' VERSION file and the release tag
# (v<version>). CHANGELOG.md has an entry for each version (the in-game
# "What's new" note is built from it).
project(SpeedBreaker VERSION 0.1.1 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Binaries name their sources from the repository root (assert messages,
# debug information), never by the folders they were built in: a published
# build carries no one's home folder (a build folder inside the checkout
# reads ./build/...). The real path too, for a checkout reached through a
# symlink; both map to the same name, so their order doesn't matter.
file(REAL_PATH "${CMAKE_SOURCE_DIR}" SB_SOURCE_REAL)
add_compile_options("-ffile-prefix-map=${CMAKE_SOURCE_DIR}=.")
if(NOT SB_SOURCE_REAL STREQUAL CMAKE_SOURCE_DIR)
    add_compile_options("-ffile-prefix-map=${SB_SOURCE_REAL}=.")
endif()

# macOS: a call to an API newer than the deployment target is an error unless
# it's checked for (@available / __builtin_available), since it would crash
# on an older macOS the app says it runs on. A diagnostic only: the code
# generated is the same.
if(APPLE AND NOT IOS)
    add_compile_options("$<$<COMPILE_LANGUAGE:C,CXX,OBJC,OBJCXX>:-Werror=unguarded-availability-new>")
endif()

add_subdirectory(recomp)
add_subdirectory(runtime)

option(SB_TESTS "Build the unit tests (tests/)" OFF)
if(SB_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
