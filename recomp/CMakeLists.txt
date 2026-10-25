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
