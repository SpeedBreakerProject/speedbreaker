// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING). See report.h.
//
// The build's identity, so every log and report says exactly which build it
// came from: the version (project() in the top-level CMakeLists.txt) and the
// commit. build_info.inc is written at build time by build_info.cmake
// (runtime/CMakeLists.txt); a build without it (a hand-run compiler) still
// works and says "unknown".
#include <stdafx.h>
#include "report.h"

#if __has_include("build_info.inc")
#include "build_info.inc"
#endif
#ifndef NFSMW_BUILD_VERSION
#define NFSMW_BUILD_VERSION "0.0.0"
#endif
#ifndef NFSMW_BUILD_COMMIT
#define NFSMW_BUILD_COMMIT "unknown"
#define NFSMW_BUILD_DIRTY 0
#define NFSMW_BUILD_DATE "at an unknown date"
#define NFSMW_BUILD_TYPE ""
#endif

namespace report
{
    const char* Version()
    {
        return NFSMW_BUILD_VERSION;
    }

    const char* VersionString()
    {
        static const std::string text = std::format("v{} ({})", Version(), BuildId());
        return text.c_str();
    }

    const char* BuildId()
    {
        static const std::string id = std::string(NFSMW_BUILD_COMMIT) + (NFSMW_BUILD_DIRTY ? "-dirty" : "");
        return id.c_str();
    }

    const char* BuildString()
    {
        static const std::string text = [] {
#if defined(__clang__)
            std::string compiler = std::format("clang {}.{}.{}", __clang_major__, __clang_minor__, __clang_patchlevel__);
#elif defined(__GNUC__)
            std::string compiler = std::format("gcc {}.{}.{}", __GNUC__, __GNUC_MINOR__, __GNUC_PATCHLEVEL__);
#else
            std::string compiler = "an unknown compiler";
#endif
#if defined(__APPLE__) && TARGET_OS_IOS
            const char* os = "iOS";
#elif defined(__APPLE__)
            const char* os = "macOS";
#elif defined(__linux__)
            const char* os = "Linux";
#elif defined(_WIN32)
            const char* os = "Windows";
#else
            const char* os = "an unknown OS";
#endif
            // The Linux build targets x86-64-v3 (AVX2): worth seeing next to
            // an "illegal instruction" crash from an older CPU.
#if defined(__aarch64__)
            const char* arch = "arm64";
#elif defined(__x86_64__) && defined(__AVX2__)
            const char* arch = "x86-64-v3";
#elif defined(__x86_64__)
            const char* arch = "x86-64";
#else
            const char* arch = "an unknown CPU";
#endif
            std::string type = NFSMW_BUILD_TYPE;
            return std::format("{}, built {}, {}, {} {}{}{}", VersionString(), NFSMW_BUILD_DATE, compiler, os, arch,
                type.empty() ? "" : ", ", type);
        }();
        return text.c_str();
    }
}
