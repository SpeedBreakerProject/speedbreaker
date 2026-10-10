# SpeedBreaker runtime. GPL-3.0-or-later (see COPYING).
set(XR ${CMAKE_CURRENT_SOURCE_DIR}/../tools/XenonRecomp)

# XEX loading (decryption, LZX) comes from XenonRecomp's XenonUtils; compile
# the few sources we need rather than linking its tool build.
add_library(XenonUtilsLite STATIC
    ${XR}/XenonUtils/xex.cpp
    ${XR}/XenonUtils/image.cpp
    ${XR}/XenonUtils/xex_patcher.cpp
    ${XR}/XenonUtils/memory_mapped_file.cpp
    ${XR}/thirdparty/libmspack/libmspack/mspack/lzxd.c
    ${XR}/thirdparty/tiny-AES-c/aes.c)
target_include_directories(XenonUtilsLite PUBLIC ${XR}/XenonUtils ${XR}/thirdparty/simde
    PRIVATE ${XR}/thirdparty/libmspack/libmspack/mspack ${XR}/thirdparty/tiny-AES-c ${XR}/thirdparty/TinySHA1)
target_compile_definitions(XenonUtilsLite PRIVATE NOMINMAX)
target_compile_options(XenonUtilsLite PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-include cstdlib>)

file(GLOB_RECURSE RUNTIME_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp)
list(FILTER RUNTIME_SOURCES EXCLUDE REGEX "/thirdparty/")
# Nayuki's QR Code generator (thirdparty/qrcodegen, vendored, C): the
# download page's code in Check for Updates. Part of the runtime itself, not
# a library of its own, so the iOS app (scripts/ios/build.sh) links it too.
list(APPEND RUNTIME_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/qrcodegen/qrcodegen.c)
if(APPLE)
    # Apple's own glue for macOS and iOS (the thermal state) is
    # Objective-C++ on Foundation alone, platform/apple/*.mm. Built with the
    # C++ compiler (else CMake picks /usr/bin/c++ next to Homebrew's clang)
    # and without ARC, like the rest.
    if(NOT CMAKE_OBJCXX_COMPILER)
        set(CMAKE_OBJCXX_COMPILER ${CMAKE_CXX_COMPILER})
    endif()
    enable_language(OBJCXX)
    file(GLOB APPLE_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/platform/apple/*.mm)
    list(APPEND RUNTIME_SOURCES ${APPLE_SOURCES})
endif()
if(IOS)
    # iOS's own glue (the Files app's picker, backups, free space) is
    # Objective-C++, platform/*.mm, compiled for iOS only. Built without ARC,
    # like the rest; its frameworks (UIKit, Foundation, Metal,
    # UniformTypeIdentifiers) are linked by scripts/ios/build.sh.
    file(GLOB IOS_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/platform/*.mm)
    list(APPEND RUNTIME_SOURCES ${IOS_SOURCES})
endif()
add_library(o1heap STATIC ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/o1heap/o1heap.c)
target_include_directories(o1heap PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/o1heap)
if(IOS)
    # On iOS the app is linked and signed by Xcode (scripts/ios): the runtime
    # is a static library there, and the app links it with the rest.
    add_library(SpeedBreaker STATIC ${RUNTIME_SOURCES})
else()
    add_executable(SpeedBreaker ${RUNTIME_SOURCES})
endif()
target_include_directories(SpeedBreaker PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/kernel)
find_package(SDL3 REQUIRED CONFIG)
if(IOS)
    # iOS has no Vulkan loader: the app links MoltenVK statically, and the
    # runtime compiles against MoltenVK's own Vulkan headers.
    set(MOLTENVK_DIR "" CACHE PATH "MoltenVK's iOS package (with MoltenVK/include and MoltenVK/static)")
    add_library(Vulkan::Vulkan INTERFACE IMPORTED)
    target_include_directories(Vulkan::Vulkan INTERFACE ${MOLTENVK_DIR}/MoltenVK/include)
else()
    find_package(Vulkan REQUIRED)
endif()
find_package(glslang REQUIRED CONFIG)
# FFmpeg for XMA audio: the minimal static build that scripts/setup_tools.sh
# makes in tools/ffmpeg-xma (with patches/ffmpeg applied: no sample hold-back
# for single-stream XMA, which the game's audio mixer depends on).
set(FFMPEG_XMA ${CMAKE_CURRENT_SOURCE_DIR}/../tools/ffmpeg-xma CACHE PATH "FFmpeg-XMA's install prefix")
find_path(FFMPEG_INCLUDE_DIR libavcodec/avcodec.h PATHS ${FFMPEG_XMA}/include NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH REQUIRED)
find_library(AVCODEC_LIBRARY NAMES libavcodec.a PATHS ${FFMPEG_XMA}/lib NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH REQUIRED)
find_library(AVUTIL_LIBRARY NAMES libavutil.a PATHS ${FFMPEG_XMA}/lib NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH REQUIRED)
# Dear ImGui (thirdparty/imgui, vendored): the installer and the settings overlay.
set(IMGUI ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/imgui)
add_library(imgui STATIC ${IMGUI}/imgui.cpp ${IMGUI}/imgui_draw.cpp ${IMGUI}/imgui_tables.cpp ${IMGUI}/imgui_widgets.cpp
    ${IMGUI}/backends/imgui_impl_sdl3.cpp ${IMGUI}/backends/imgui_impl_vulkan.cpp)
target_include_directories(imgui PUBLIC ${IMGUI} ${IMGUI}/backends)
target_link_libraries(imgui PUBLIC SDL3::SDL3 Vulkan::Vulkan)
# The UI font, compiled in as a byte array (generated/ui_font.inc).
file(READ ${IMGUI}/fonts/Roboto-Medium.ttf UI_FONT_HEX HEX)
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," UI_FONT_BYTES "${UI_FONT_HEX}")
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/generated/ui_font.inc "${UI_FONT_BYTES}\n")
target_include_directories(SpeedBreaker PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/generated)
# The "What's new" note (ui/whats_new.cpp): this version's entry in
# CHANGELOG.md, from its "## v<version>" heading to the next "## " one,
# compiled in (generated/whats_new.inc). An edit of CHANGELOG.md reconfigures.
set(SB_CHANGELOG ${CMAKE_SOURCE_DIR}/CHANGELOG.md)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${SB_CHANGELOG})
set(SB_WHATS_NEW "")
if(EXISTS ${SB_CHANGELOG})
    file(READ ${SB_CHANGELOG} changelog)
    string(REPLACE "\r\n" "\n" changelog "\n${changelog}")
    string(FIND "${changelog}" "\n## v${CMAKE_PROJECT_VERSION}" at)
    if(at GREATER_EQUAL 0)
        string(LENGTH "\n## v${CMAKE_PROJECT_VERSION}" heading)
        math(EXPR after "${at} + ${heading}")
        string(SUBSTRING "${changelog}" ${after} -1 entry)
        # The heading's end ("## v0.1.0" or "## v0.1.0 (2026-10-08)", never v0.1.01).
        string(REGEX MATCH "^([ \t(][^\n]*)?\n" rest "${entry}")
        if(rest STREQUAL "" AND NOT entry STREQUAL "")
            set(at -1)
        else()
            string(LENGTH "${rest}" restLength)
            string(SUBSTRING "${entry}" ${restLength} -1 entry)
            string(FIND "${entry}" "\n## " next)
            if(next GREATER_EQUAL 0)
                string(SUBSTRING "${entry}" 0 ${next} entry)
            endif()
            string(STRIP "${entry}" SB_WHATS_NEW)
        endif()
    endif()
endif()
if(SB_WHATS_NEW STREQUAL "")
    message(WARNING "CHANGELOG.md has no entry for v${CMAKE_PROJECT_VERSION}: this build shows no \"What's new\" note")
endif()
string(FIND "${SB_WHATS_NEW}" ")sbnote\"" bad)
if(bad GREATER_EQUAL 0)
    message(FATAL_ERROR "CHANGELOG.md's v${CMAKE_PROJECT_VERSION} entry contains )sbnote\"")
endif()
set(whats_new_inc "// Generated from CHANGELOG.md by runtime/CMakeLists.txt. Do not edit.\nconstexpr const char kWhatsNewEntry[] = R\"sbnote(${SB_WHATS_NEW})sbnote\";\n")
set(whats_new_file ${CMAKE_CURRENT_BINARY_DIR}/generated/whats_new.inc)
set(whats_new_old "")
if(EXISTS ${whats_new_file})
    file(READ ${whats_new_file} whats_new_old)
endif()
if(NOT whats_new_old STREQUAL whats_new_inc)
    file(WRITE ${whats_new_file} "${whats_new_inc}")
endif()
# The build's identity (version, git commit, dirty flag, build date) for the
# logs, crash reports and Settings > Advanced: generated/build_info.inc,
# checked on every build but rewritten only when it changes
# (report/build_info.cmake), so a no-op build stays one.
add_custom_target(nfsmw_build_info
    COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${CMAKE_SOURCE_DIR} -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/generated/build_info.inc
        -DVERSION=${CMAKE_PROJECT_VERSION} -DBUILD_TYPE=${CMAKE_BUILD_TYPE} -P ${CMAKE_CURRENT_SOURCE_DIR}/report/build_info.cmake
    BYPRODUCTS ${CMAKE_CURRENT_BINARY_DIR}/generated/build_info.inc
    VERBATIM)
add_dependencies(SpeedBreaker nfsmw_build_info)

target_link_libraries(SpeedBreaker PRIVATE NfsmwRecompLib XenonUtilsLite o1heap imgui SDL3::SDL3 Vulkan::Vulkan glslang::glslang glslang::glslang-default-resource-limits ${AVCODEC_LIBRARY} ${AVUTIL_LIBRARY})
# glslang before 15 (the Steam Frame's 14) keeps the C API's SPIR-V
# generation in its own library.
if(TARGET glslang::SPIRV)
    target_link_libraries(SpeedBreaker PRIVATE glslang::SPIRV)
endif()
# platform/apple/*.mm (iOS: linked by scripts/ios/build.sh): Foundation, and
# on macOS AppKit (file_dialog.mm) and CoreFoundation (install/placement.cpp:
# is a folder in iCloud Drive).
if(APPLE AND NOT IOS)
    target_link_libraries(SpeedBreaker PRIVATE "-framework Foundation" "-framework AppKit" "-framework CoreFoundation")
endif()
target_include_directories(SpeedBreaker SYSTEM PRIVATE ${FFMPEG_INCLUDE_DIR} ${XR}/thirdparty/tomlplusplus/include)
# Assertions stay on in every build type: ported code marks its unhandled
# cases with assert(), and those must fail loudly, never fall through.
target_compile_options(SpeedBreaker PRIVATE -Wno-unused-label -Wno-unused-variable -fno-strict-aliasing -UNDEBUG)
if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
    target_compile_options(SpeedBreaker PRIVATE -march=x86-64-v3)
endif()
if(IOS)
    # The CPU level both the iPad Pro (M2) and the iPhone 15 Pro (A17 Pro)
    # have; Apple clang's iOS default is far older.
    target_compile_options(SpeedBreaker PRIVATE -mcpu=apple-a15)
endif()
# macOS: the build folder gets MoltenVK's manifest too, next to the game
# (runtime/vulkan/icd.d/MoltenVK_icd.json), naming the MoltenVK that
# scripts/macos/make_app.py bundles (MOLTENVK in the environment, else
# Homebrew's molten-vk; -DSB_MOLTENVK=<libMoltenVK.dylib> to choose). The game
# points the Vulkan loader at <its folder>/vulkan/icd.d/MoltenVK_icd.json when
# there is one, as in SpeedBreaker.app (video/presenter.cpp). The loader
# scripts/macos/build_deps.sh builds searches only the system's folders
# (/usr/local/etc, /etc, ...), never Homebrew's, where molten-vk keeps its
# manifest: without this, the game run from the build folder (and
# scripts/bench.sh, test_suspend.sh, verify_settings.py) finds no Vulkan
# driver. VK_DRIVER_FILES or VK_ICD_FILENAMES set by hand still win.
if(APPLE AND NOT IOS)
    if(DEFINED ENV{MOLTENVK})
        set(_sb_moltenvk "$ENV{MOLTENVK}")
    else()
        set(_sb_moltenvk "/opt/homebrew/opt/molten-vk/lib/libMoltenVK.dylib")
    endif()
    set(SB_MOLTENVK "${_sb_moltenvk}" CACHE FILEPATH "The MoltenVK the game in the build folder uses")
    if(EXISTS "${SB_MOLTENVK}")
        file(GENERATE OUTPUT "$<TARGET_FILE_DIR:SpeedBreaker>/vulkan/icd.d/MoltenVK_icd.json"
            CONTENT "{\n    \"file_format_version\": \"1.0.0\",\n    \"ICD\": {\n        \"library_path\": \"${SB_MOLTENVK}\",\n        \"api_version\": \"1.4.0\",\n        \"is_portability_driver\": true\n    }\n}\n")
    else()
        message(STATUS "No MoltenVK at ${SB_MOLTENVK}: the game in the build folder takes the Vulkan loader's own drivers (-DSB_MOLTENVK=<libMoltenVK.dylib> names one)")
    endif()
endif()
