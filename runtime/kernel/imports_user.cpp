// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING).
//
// XAM user/profile exports for the single local, signed-in user 0.
// Semantics follow Xenia (BSD): kernel/xam/xam_user.cc and user_profile.cc.
// Title-specific profile settings (the blobs games keep their options in)
// are stored and persisted to <user path>/profile_settings.bin.
#include <stdafx.h>
#include <kernel/function.h>
#include <kernel/overlapped.h>
#include <user/paths.h>

#include <fstream>
#include <map>

namespace
{
    constexpr uint32_t X_ERROR_SUCCESS = 0;
    constexpr uint32_t X_ERROR_INVALID_PARAMETER = 0x57;
    constexpr uint32_t X_ERROR_INSUFFICIENT_BUFFER = 0x7A;
    constexpr uint32_t X_ERROR_IO_PENDING = 0x3E5;
    constexpr uint32_t X_ERROR_NO_SUCH_USER = 0x525;

    constexpr uint64_t kXuid = 0xB13EBABEBABEBABEull;  // same XUID as XamUserGetSigninInfo

    enum SettingType : uint32_t { CONTENT = 0, INT32 = 1, INT64 = 2, DOUBLE = 3, WSTRING = 4, FLOAT = 5, BINARY = 6, DATETIME = 7 };

    uint32_t SettingType(uint32_t id) { return id >> 28; }
    uint32_t SettingSize(uint32_t id) { return (id >> 16) & 0xFFF; }
    bool IsTitleSpecific(uint32_t id) { return (id & 0x3F00) == 0x3F00; }

    struct SettingData
    {
        uint8_t type;
        uint8_t unk1[3];
        be<uint32_t> unk4;
        union
        {
            be<int32_t> s32;
            be<int64_t> s64;
            be<uint32_t> u32;
            be<float> f32;
            struct { be<uint32_t> size, ptr; } blob;
        };
    };
    static_assert(sizeof(SettingData) == 16);

    struct ProfileSetting
    {
        be<uint32_t> from;
        be<uint32_t> unk04;
        union
        {
            be<uint32_t> userIndex;
            be<uint64_t> xuid;
        };
        be<uint32_t> settingId;
        be<uint32_t> unk14;
        SettingData data;
    };
    static_assert(sizeof(ProfileSetting) == 40);

    struct ReadProfileSettings
    {
        be<uint32_t> settingCount;
        be<uint32_t> settingsPtr;
    };

    // Xenia's default profile (user_profile.cc): integer/float settings.
    const std::map<uint32_t, uint32_t> kDefaults = {
        { 0x10040002, 0 },          // XPROFILE_GAMER_YAXIS_INVERSION
        { 0x10040003, 3 },          // XPROFILE_OPTION_CONTROLLER_VIBRATION
        { 0x10040004, 0 },          // XPROFILE_GAMERCARD_ZONE
        { 0x10040005, 0 },          // XPROFILE_GAMERCARD_REGION
        { 0x10040006, 0xFA },       // XPROFILE_GAMERCARD_CRED
        { 0x5004000B, 0 },          // XPROFILE_GAMERCARD_REP (float 0)
        { 0x1004000C, 0 },          // XPROFILE_OPTION_VOICE_MUTED
        { 0x1004000D, 0 },          // XPROFILE_OPTION_VOICE_THRU_SPEAKERS
        { 0x1004000E, 0x64 },       // XPROFILE_OPTION_VOICE_VOLUME
        { 0x10040012, 1 },          // XPROFILE_GAMERCARD_TITLES_PLAYED
        { 0x10040013, 0 },          // XPROFILE_GAMERCARD_ACHIEVEMENTS_EARNED
        { 0x10040015, 0 },          // XPROFILE_GAMER_DIFFICULTY
        { 0x10040018, 0 },          // XPROFILE_GAMER_CONTROL_SENSITIVITY
        { 0x1004001D, 0xFFFF0000 }, // XPROFILE_GAMER_PREFERRED_COLOR_FIRST
        { 0x1004001E, 0xFF00FF00 }, // XPROFILE_GAMER_PREFERRED_COLOR_SECOND
        { 0x10040022, 1 },          // XPROFILE_GAMER_ACTION_AUTO_AIM
        { 0x10040023, 0 },          // XPROFILE_GAMER_ACTION_AUTO_CENTER
        { 0x10040024, 0 },          // XPROFILE_GAMER_ACTION_MOVEMENT_CONTROL
        { 0x10040026, 0 },          // XPROFILE_GAMER_RACE_TRANSMISSION
        { 0x10040027, 0 },          // XPROFILE_GAMER_RACE_CAMERA_LOCATION
        { 0x10040028, 0 },          // XPROFILE_GAMER_RACE_BRAKE_CONTROL
        { 0x10040029, 0 },          // XPROFILE_GAMER_RACE_ACCELERATOR_CONTROL
        { 0x10040038, 0 },          // XPROFILE_GAMERCARD_TITLE_CRED_EARNED
        { 0x10040039, 0 },          // XPROFILE_GAMERCARD_TITLE_ACHIEVEMENTS_EARNED
    };

    std::mutex s_mutex;
    std::map<uint32_t, std::vector<uint8_t>> s_blobs;  // title-specific binary settings
    bool s_loaded = false;

    std::filesystem::path BlobPath()
    {
        return GetUserPath() / "profile_settings.bin";
    }

    // File: repeated { u32 id, u32 size, bytes } (host order).
    void LoadBlobs()
    {
        if (s_loaded)
            return;
        s_loaded = true;
        std::ifstream f(BlobPath(), std::ios::binary);
        uint32_t id, size;
        while (f.read(reinterpret_cast<char*>(&id), 4) && f.read(reinterpret_cast<char*>(&size), 4))
        {
            if (size > 0x10000)
                break;
            std::vector<uint8_t> data(size);
            if (!f.read(reinterpret_cast<char*>(data.data()), size))
                break;
            s_blobs[id] = std::move(data);
        }
    }

    void SaveBlobs()
    {
        std::error_code ec;
        std::filesystem::create_directories(GetUserPath(), ec);
        std::filesystem::path tmp = BlobPath();
        tmp += ".tmp";
        {
            std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
            for (const auto& [id, data] : s_blobs)
            {
                uint32_t size = uint32_t(data.size());
                f.write(reinterpret_cast<const char*>(&id), 4);
                f.write(reinterpret_cast<const char*>(&size), 4);
                f.write(reinterpret_cast<const char*>(data.data()), size);
            }
        }
        std::filesystem::rename(tmp, BlobPath(), ec);
        if (ec)
            fprintf(stderr, "[profile] saving %s failed: %s\n", BlobPath().string().c_str(), ec.message().c_str());
    }

    uint32_t Finish(XXOVERLAPPED* overlapped, uint32_t result)
    {
        if (overlapped)
        {
            CompleteOverlapped(overlapped, result);
            return X_ERROR_IO_PENDING;
        }
        return result;
    }
}

uint32_t XamUserReadProfileSettings(uint32_t titleId, uint32_t userIndex, uint32_t xuidCount, be<uint64_t>* xuids,
    uint32_t settingCount, be<uint32_t>* settingIds, be<uint32_t>* bufferSize, uint8_t* buffer, XXOVERLAPPED* overlapped)
{
    if (settingCount < 1 || settingCount > 32 || !bufferSize)
        return X_ERROR_INVALID_PARAMETER;
    uint32_t size = *bufferSize;
    if (size && !buffer)
        return X_ERROR_INVALID_PARAMETER;
    uint32_t records = std::max(1u, xuidCount);

    std::lock_guard lock(s_mutex);
    LoadBlobs();
    uint32_t headerSize = settingCount * sizeof(ProfileSetting);
    uint32_t dataSize = 0;
    for (uint32_t i = 0; i < settingCount; i++)
    {
        uint32_t id = settingIds[i];
        if (SettingType(id) == WSTRING || SettingType(id) == BINARY)
            dataSize += SettingSize(id);
    }
    headerSize = headerSize * records + sizeof(ReadProfileSettings);
    dataSize *= records;
    uint32_t needed = headerSize + dataSize;
    if (!buffer || size < needed)
    {
        if (!size)
            *bufferSize = needed;
        return X_ERROR_INSUFFICIENT_BUFFER;
    }
    if (!xuids && userIndex != 0)
        return Finish(overlapped, X_ERROR_NO_SUCH_USER);

    auto* header = reinterpret_cast<ReadProfileSettings*>(buffer);
    auto* out = reinterpret_cast<ProfileSetting*>(header + 1);
    header->settingCount = settingCount * records;
    header->settingsPtr = g_memory.MapVirtual(out);
    uint32_t dataOffset = headerSize;
    for (uint32_t r = 0; r < records; r++)
    {
        for (uint32_t i = 0; i < settingCount; i++, out++)
        {
            uint32_t id = settingIds[i];
            memset(out, 0, sizeof(*out));
            out->settingId = id;
            if (xuids)
                out->xuid = kXuid;
            else
                out->userIndex = userIndex;
            uint32_t type = SettingType(id);
            out->data.type = uint8_t(type);
            if (auto it = kDefaults.find(id); it != kDefaults.end())
            {
                out->from = 1;
                out->data.u32 = it->second;
            }
            else if (type == BINARY || type == WSTRING)
            {
                auto blob = s_blobs.find(id);
                if (blob != s_blobs.end())
                {
                    uint32_t n = std::min<uint32_t>(uint32_t(blob->second.size()), SettingSize(id));
                    memcpy(buffer + dataOffset, blob->second.data(), n);
                    out->from = IsTitleSpecific(id) ? 2 : 1;
                    out->data.blob.size = n;
                    out->data.blob.ptr = g_memory.MapVirtual(buffer + dataOffset);
                    dataOffset += SettingSize(id);
                }
                // else: not set (from = 0), as a fresh profile
            }
            else
            {
                fprintf(stderr, "[profile] read of unknown setting %08X (reported unset)\n", id);
            }
        }
    }
    return Finish(overlapped, X_ERROR_SUCCESS);
}

uint32_t XamUserWriteProfileSettings(uint32_t titleId, uint32_t userIndex, uint32_t settingCount,
    ProfileSetting* settings, XXOVERLAPPED* overlapped)
{
    if (settingCount == 0 || !settings)
        return X_ERROR_INVALID_PARAMETER;
    if (userIndex != 0)
        return Finish(overlapped, X_ERROR_NO_SUCH_USER);
    std::lock_guard lock(s_mutex);
    LoadBlobs();
    bool changed = false;
    for (uint32_t i = 0; i < settingCount; i++)
    {
        uint32_t id = settings[i].settingId;
        uint32_t type = settings[i].data.type;
        if (type == BINARY || type == WSTRING)
        {
            uint32_t n = std::min<uint32_t>(settings[i].data.blob.size, SettingSize(id));
            const uint8_t* src = static_cast<const uint8_t*>(g_memory.Translate(settings[i].data.blob.ptr));
            std::vector<uint8_t> data(src, src + n);
            auto& slot = s_blobs[id];
            if (slot != data)
            {
                slot = std::move(data);
                changed = true;
            }
        }
        else
        {
            fprintf(stderr, "[profile] write of setting %08X type %u ignored\n", id, type);
        }
    }
    if (changed)
        SaveBlobs();
    return Finish(overlapped, X_ERROR_SUCCESS);
}

uint32_t XamUserGetXUID(uint32_t userIndex, uint32_t typeMask, be<uint64_t>* xuid)
{
    if (userIndex != 0)
        return X_ERROR_NO_SUCH_USER;
    *xuid = kXuid;
    return X_ERROR_SUCCESS;
}

uint32_t XamUserGetName(uint32_t userIndex, char* buffer, uint32_t bufferLength)
{
    if (userIndex != 0)
        return X_ERROR_NO_SUCH_USER;
    if (!buffer || bufferLength == 0)
        return X_ERROR_INVALID_PARAMETER;
    snprintf(buffer, bufferLength, "%s", "Player");
    return X_ERROR_SUCCESS;
}

// Offline: deny every privilege, as Xenia does (online play, voice,
// purchases and user content are all gated behind them).
uint32_t XamUserCheckPrivilege(uint32_t userIndex, uint32_t privilege, be<uint32_t>* result)
{
    if (userIndex != 0 && userIndex != 0xFF)
        return X_ERROR_NO_SUCH_USER;
    *result = 0;
    return X_ERROR_SUCCESS;
}

GUEST_FUNCTION_HOOK(__imp__XamUserReadProfileSettings, XamUserReadProfileSettings);
GUEST_FUNCTION_HOOK(__imp__XamUserWriteProfileSettings, XamUserWriteProfileSettings);
GUEST_FUNCTION_HOOK(__imp__XamUserGetXUID, XamUserGetXUID);
GUEST_FUNCTION_HOOK(__imp__XamUserGetName, XamUserGetName);
GUEST_FUNCTION_HOOK(__imp__XamUserCheckPrivilege, XamUserCheckPrivilege);
