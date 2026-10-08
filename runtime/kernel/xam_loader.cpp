// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING).
//
// XAM loader: launch data passed between titles, title termination. Follows
// Xenia's xam_info.cc: without launch data the getters report
// ERROR_NOT_FOUND; data set by the title is kept for a later relaunch.
#include <stdafx.h>
#include "function.h"
#include <report/report.h>

namespace
{
    constexpr uint32_t X_ERROR_SUCCESS = 0;
    constexpr uint32_t X_ERROR_NOT_FOUND = 0x490;
    constexpr uint32_t X_E_INVALIDARG = 0x80070057;

    std::mutex s_mutex;
    std::vector<uint8_t> s_launchData;
    bool s_launchDataPresent = false;
}

uint32_t XamLoaderGetLaunchDataSize(be<uint32_t>* size)
{
    if (size == nullptr)
        return X_E_INVALIDARG;
    std::lock_guard lock(s_mutex);
    *size = s_launchDataPresent ? uint32_t(s_launchData.size()) : 0;
    return s_launchDataPresent ? X_ERROR_SUCCESS : X_ERROR_NOT_FOUND;
}

uint32_t XamLoaderGetLaunchData(uint8_t* buffer, uint32_t bufferSize)
{
    std::lock_guard lock(s_mutex);
    if (!s_launchDataPresent)
        return X_ERROR_NOT_FOUND;
    memcpy(buffer, s_launchData.data(), std::min<size_t>(bufferSize, s_launchData.size()));
    return X_ERROR_SUCCESS;
}

uint32_t XamLoaderSetLaunchData(const uint8_t* data, uint32_t size)
{
    std::lock_guard lock(s_mutex);
    s_launchData.assign(data, data + size);
    s_launchDataPresent = true;
    return X_ERROR_SUCCESS;
}

void XamLoaderTerminateTitle()
{
    fprintf(stderr, "[xam] the title asked to terminate; exiting\n");
    fflush(stderr);
    report::FlushLog();  // into the session log before _Exit
    std::_Exit(0);
}

void XamLoaderLaunchTitle(const char* path, uint32_t flags)
{
    fprintf(stderr, "[xam] the title asked to launch \"%s\" (flags %08X); relaunching isn't supported, exiting\n",
        path ? path : "(null)", flags);
    fflush(stderr);
    report::FlushLog();
    std::_Exit(0);
}

GUEST_FUNCTION_HOOK(__imp__XamLoaderGetLaunchDataSize, XamLoaderGetLaunchDataSize);
GUEST_FUNCTION_HOOK(__imp__XamLoaderGetLaunchData, XamLoaderGetLaunchData);
GUEST_FUNCTION_HOOK(__imp__XamLoaderSetLaunchData, XamLoaderSetLaunchData);
GUEST_FUNCTION_HOOK(__imp__XamLoaderTerminateTitle, XamLoaderTerminateTitle);
GUEST_FUNCTION_HOOK(__imp__XamLoaderLaunchTitle, XamLoaderLaunchTitle);
