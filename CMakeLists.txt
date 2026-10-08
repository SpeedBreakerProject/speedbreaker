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
# the game runs on, which the Mac app declares (scripts/macos/make_app.py
# reads it from the binaries and refuses a library that needs a newer one).
# Homebrew's bottles for macOS 26 are built for 26.0, so that's the floor
# while the app bundles them. Before project(): it picks the SDK's settings.
if(CMAKE_HOST_APPLE AND NOT CMAKE_SYSTEM_NAME STREQUAL "iOS" AND NOT DEFINED CMAKE_OSX_DEPLOYMENT_TARGET)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "26.0" CACHE STRING "The oldest macOS the game runs on")
endif()

# The version: the one place it is set. Everything else comes from it: the
# log's [build] line, Settings > Advanced, bug reports' names, the Mac and iOS
# apps' versions, the SteamOS bundles' VERSION file and the release tag
# (v<version>). CHANGELOG.md has an entry for each version (the in-game
# "What's new" note is built from it).
project(SpeedBreaker VERSION 0.1.0 LANGUAGES C CXX)

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

add_subdirectory(recomp)
add_subdirectory(runtime)

option(SB_TESTS "Build the unit tests (tests/)" OFF)
if(SB_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
