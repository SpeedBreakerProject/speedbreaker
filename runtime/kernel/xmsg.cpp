// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING).
//
// XAM message apps (XMsg*). Only XMP (0xFA, the system music player, which
// games ask about custom soundtracks) is implemented, following Xenia's
// kernel/xam/apps/xmp_app.cc message by message. XGI (0xFB: achievements,
// sessions, stats) and XLiveBase (0xFC) messages fail with X_E_FAIL and a log
// line, as Xenia's unknown messages do; add them as the game needs them.
#include <stdafx.h>
#include "function.h"
#include "overlapped.h"
#include "xam.h"

#include <cpu/guest_thread.h>

namespace
{
    constexpr uint32_t X_E_SUCCESS = 0;
    constexpr uint32_t X_E_FAIL = 0x80004005;
    constexpr uint32_t X_E_INVALIDARG = 0x80070057;
    constexpr uint32_t X_E_NOTFOUND = 0x80070490;
    constexpr uint32_t X_ERROR_IO_PENDING = 997;
    constexpr uint32_t X_ERROR_NOT_FOUND = 1168;

    // XMP notifications (XN_XMP_*).
    constexpr uint32_t kMsgStateChanged = 0x0A000001;
    constexpr uint32_t kMsgPlaybackBehaviorChanged = 0x0A000002;
    constexpr uint32_t kMsgPlaybackControllerChanged = 0x0A000003;

    struct Xmp
    {
        std::mutex mutex;
        uint32_t state = 0;             // 0 idle/stopped, 1 playing, 2 paused
        uint32_t playbackClient = 1;    // the title controls playback
        uint32_t playbackMode = 0;
        uint32_t repeatMode = 0;
        uint32_t unknownFlags = 0;
        float volume = 1.0f;
        std::unordered_map<uint32_t, uint32_t> playlists;  // handle -> song count
    } s_xmp;

    template<typename T>
    T* Guest(uint32_t address) { return static_cast<T*>(g_memory.Translate(address)); }

    uint32_t XmpDispatch(uint32_t message, uint32_t buffer, uint32_t length)
    {
        std::lock_guard lock(s_xmp.mutex);
        auto* args = Guest<be<uint32_t>>(buffer);
        switch (message)
        {
        case 0x00070002: // XMPPlayTitlePlaylist(client, storage, song)
        {
            uint32_t handle = *Guest<be<uint32_t>>(args[1]);
            if (!s_xmp.playlists.count(handle))
                return X_E_NOTFOUND;
            // Custom soundtracks aren't played; the title keeps its own music.
            fprintf(stderr, "[xmp] PlayTitlePlaylist %08X (not played)\n", handle);
            s_xmp.state = 1;
            XamNotifyEnqueueEvent(kMsgStateChanged, s_xmp.state);
            return X_E_SUCCESS;
        }
        case 0x00070003: // XMPContinue
            if (s_xmp.state == 2) s_xmp.state = 1;
            XamNotifyEnqueueEvent(kMsgStateChanged, s_xmp.state);
            return X_E_SUCCESS;
        case 0x00070004: // XMPStop
            s_xmp.state = 0;
            XamNotifyEnqueueEvent(kMsgStateChanged, s_xmp.state);
            return X_E_SUCCESS;
        case 0x00070005: // XMPPause
            if (s_xmp.state == 1) s_xmp.state = 2;
            XamNotifyEnqueueEvent(kMsgStateChanged, s_xmp.state);
            return X_E_SUCCESS;
        case 0x00070006: // XMPNext
        case 0x00070007: // XMPPrevious
            return X_E_NOTFOUND;
        case 0x00070008: // XMPSetPlaybackBehavior(client, mode, repeat, flags)
            s_xmp.playbackMode = args[1];
            s_xmp.repeatMode = args[2];
            s_xmp.unknownFlags = args[3];
            XamNotifyEnqueueEvent(kMsgPlaybackBehaviorChanged, 0);
            return X_E_SUCCESS;
        case 0x00070009: // XMPGetStatus(client, state*)
            *Guest<be<uint32_t>>(args[1]) = s_xmp.state;
            return X_E_SUCCESS;
        case 0x0007000B: // XMPGetVolume(client, float*)
            *Guest<be<float>>(args[1]) = s_xmp.volume;
            return X_E_SUCCESS;
        case 0x0007000C: // XMPSetVolume(client, float)
            s_xmp.volume = reinterpret_cast<be<float>*>(args)[1];
            return X_E_SUCCESS;
        case 0x0007000D: // XMPCreateTitlePlaylist(client, storage, size, songs, count, name, flags, songHandles*, handle*)
        {
            uint32_t storage = args[1];
            *Guest<be<uint32_t>>(args[8]) = storage;  // the handle is the storage pointer (Xenia)
            *Guest<be<uint32_t>>(storage) = storage;
            s_xmp.playlists[storage] = args[4];
            return X_E_SUCCESS;
        }
        case 0x0007000E: // XMPGetInfo: nothing is ever playing
            return X_E_FAIL;
        case 0x00070013: // XMPDeleteTitlePlaylist(client, storage)
            return s_xmp.playlists.erase(*Guest<be<uint32_t>>(args[1])) ? X_E_SUCCESS : X_E_NOTFOUND;
        case 0x0007001A: // XMPSetPlaybackController(client, controller, playbackClient)
            s_xmp.playbackClient = args[2];
            XamNotifyEnqueueEvent(kMsgPlaybackControllerChanged, !uint32_t(args[2]));
            return X_E_SUCCESS;
        case 0x0007001B: // XMPGetPlaybackController(client, controller*, locked*)
            *Guest<be<uint32_t>>(args[1]) = 0;
            *Guest<be<uint32_t>>(args[2]) = 0;
            return X_E_SUCCESS;
        case 0x00070029: // XMPGetPlaybackBehavior(client, mode*, repeat*, flags*)
            if (args[1]) *Guest<be<uint32_t>>(args[1]) = s_xmp.playbackMode;
            if (args[2]) *Guest<be<uint32_t>>(args[2]) = s_xmp.repeatMode;
            if (args[3]) *Guest<be<uint32_t>>(args[3]) = s_xmp.unknownFlags;
            return X_E_SUCCESS;
        case 0x0007002E: // storage size for a playlist of N songs
            *Guest<be<uint32_t>>(args[2]) = 4 + uint32_t(args[1]) * 128;
            return X_E_SUCCESS;
        }
        fprintf(stderr, "[xmp] unimplemented message %08X (buffer %08X, %u bytes)\n", message, buffer, length);
        return X_E_FAIL;
    }

    uint32_t Dispatch(uint32_t app, uint32_t message, uint32_t buffer, uint32_t length)
    {
        if (app == 0xFA)
            return XmpDispatch(message, buffer, length);
        if (app == 0xFB || app == 0xFC)
        {
            fprintf(stderr, "[xmsg] app %02X message %08X not implemented (returning X_E_FAIL)\n", app, message);
            return X_E_FAIL;
        }
        return X_E_NOTFOUND;
    }
}

uint32_t XMsgInProcessCall(uint32_t app, uint32_t message, uint32_t arg1, uint32_t arg2)
{
    return Dispatch(app, message, arg1, arg2);
}

uint32_t XMsgSystemProcessCall(uint32_t app, uint32_t message, uint32_t buffer, uint32_t length)
{
    return Dispatch(app, message, buffer, length);
}

uint32_t XMsgStartIORequest(uint32_t app, uint32_t message, XXOVERLAPPED* overlapped, uint32_t buffer, uint32_t length)
{
    uint32_t result = Dispatch(app, message, buffer, length);
    if (result == X_E_NOTFOUND)
    {
        fprintf(stderr, "[xmsg] XMsgStartIORequest: app %08X undefined\n", app);
        result = X_E_INVALIDARG;
        GuestThread::SetLastError(X_ERROR_NOT_FOUND);
    }
    if (overlapped != nullptr)
    {
        CompleteOverlapped(overlapped, result);
        result = X_ERROR_IO_PENDING;
    }
    if (result == 0 || result == X_ERROR_IO_PENDING)
        GuestThread::SetLastError(0);
    return result;
}

uint32_t XMsgCancelIORequest(XXOVERLAPPED* overlapped, uint32_t wait)
{
    return 0;  // every request completes immediately
}

GUEST_FUNCTION_HOOK(__imp__XMsgInProcessCall, XMsgInProcessCall);
GUEST_FUNCTION_HOOK(__imp__XMsgSystemProcessCall, XMsgSystemProcessCall);
GUEST_FUNCTION_HOOK(__imp__XMsgStartIORequest, XMsgStartIORequest);
GUEST_FUNCTION_HOOK(__imp__XMsgCancelIORequest, XMsgCancelIORequest);
