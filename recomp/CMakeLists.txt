# Compiles XenonRecomp's output (../ppc, generated from the user's own XEX and
# never committed) into a static library. Regenerate ppc/ with:
#   tools/XenonRecomp/build/XenonRecomp/XenonRecomp config/nfsmw.toml tools/XenonRecomp/XenonUtils/ppc_context.h
set(ROOT ${CMAKE_CURRENT_SOURCE_DIR}/..)
# Built from the top-level CMakeLists.txt.
file(GLOB PPC_SOURCES ${ROOT}/ppc/*.cpp)
if(NOT PPC_SOURCES)
    message(FATAL_ERROR "No generated sources in ppc/. Run XenonRecomp first.")
endif()

add_library(NfsmwRecompLib STATIC ${PPC_SOURCES})
target_compile_features(NfsmwRecompLib PUBLIC cxx_std_20)
target_include_directories(NfsmwRecompLib PUBLIC
    ${ROOT}/ppc
    ${ROOT}/tools/XenonRecomp/XenonUtils
    ${ROOT}/tools/XenonRecomp/thirdparty/simde)
target_compile_options(NfsmwRecompLib PRIVATE
    -Wno-unused-label -Wno-unused-variable -Wno-unused-but-set-variable
    -fno-strict-aliasing)
if(SB_MSVC_FRONTEND)
    # clang-cl has no -include: /FI, in the same order. The third, on
    # Windows only, aligns the guest's jmp_bufs for the CRT's setjmp
    # (runtime/platform/win32/guest_setjmp.h).
    target_compile_options(NfsmwRecompLib PRIVATE
        "/FI${ROOT}/runtime/cpu/indirect.h"
        "/FI${ROOT}/runtime/cpu/guest_time.h"
        "/FI${ROOT}/runtime/platform/win32/guest_setjmp.h")
else()
    target_compile_options(NfsmwRecompLib PRIVATE
        # Checked indirect calls (runtime/cpu/indirect.cpp). SHELL: keeps each
        # -include with its file (CMake drops a repeated plain -include).
        "SHELL:-include \"${ROOT}/runtime/cpu/indirect.h\""
        # mftb reads the game's clock, which stands still while it's suspended.
        "SHELL:-include \"${ROOT}/runtime/cpu/guest_time.h\"")
endif()
if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
    if(SB_MSVC_FRONTEND)
        target_compile_options(NfsmwRecompLib PRIVATE /clang:-march=x86-64-v3)
    else()
        target_compile_options(NfsmwRecompLib PRIVATE -march=x86-64-v3)
    endif()
endif()
if(IOS)
    target_compile_options(NfsmwRecompLib PRIVATE -mcpu=apple-a15)
endif()

# The European (PAL, Spanish) release's code (../ppc-pal-es: XenonRecomp with
# config/pal-es/nfsmw.toml on that disc's default.xex, then
# scripts/region_ppc.py ppc-pal-es pal_es), linked beside the USA's under
# namespace pal_es; the runtime picks one at startup (runtime/region/region.h).
# SB_PAL_ES: AUTO (when ppc-pal-es/ is there), ON (required) or OFF.
set(SB_PAL_ES AUTO CACHE STRING "Link the PAL (Spanish) release's recompiled code: AUTO, ON or OFF")
file(GLOB PPC_PAL_ES_SOURCES ${ROOT}/ppc-pal-es/*.cpp)
if(SB_PAL_ES STREQUAL "ON" AND NOT PPC_PAL_ES_SOURCES)
    message(FATAL_ERROR "SB_PAL_ES=ON, but ppc-pal-es/ has no generated sources")
endif()
if(PPC_PAL_ES_SOURCES AND NOT SB_PAL_ES STREQUAL "OFF")
    file(READ ${ROOT}/ppc-pal-es/ppc_recomp_shared.h _sb_pal_head LIMIT 64)
    if(NOT _sb_pal_head MATCHES "^// Namespaced by scripts/region_ppc.py: pal_es")
        message(FATAL_ERROR "ppc-pal-es/ isn't namespaced: run python3 scripts/region_ppc.py ppc-pal-es pal_es")
    endif()
    add_library(NfsmwRecompPalEsLib STATIC ${PPC_PAL_ES_SOURCES})
    target_compile_features(NfsmwRecompPalEsLib PUBLIC cxx_std_20)
    target_include_directories(NfsmwRecompPalEsLib PRIVATE
        ${ROOT}/ppc-pal-es
        ${ROOT}/tools/XenonRecomp/XenonUtils
        ${ROOT}/tools/XenonRecomp/thirdparty/simde)
    get_target_property(_sb_ppc_options NfsmwRecompLib COMPILE_OPTIONS)
    target_compile_options(NfsmwRecompPalEsLib PRIVATE ${_sb_ppc_options})
    set(SB_HAVE_PAL_ES ON CACHE INTERNAL "ppc-pal-es is linked")
    message(STATUS "PAL (Spanish) release: linked (ppc-pal-es/)")
else()
    set(SB_HAVE_PAL_ES OFF CACHE INTERNAL "ppc-pal-es is linked")
    message(STATUS "PAL (Spanish) release: not linked (no ppc-pal-es/, or SB_PAL_ES=OFF)")
endif()
