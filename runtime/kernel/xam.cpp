// Adapted from Unleashed Recompiled (https://github.com/hedge-dev/UnleashedRecomp),
// GPL-3.0-or-later. Modified for SpeedBreaker: silent stubs and the input (hid/SDL)
// exports are removed; input returns in Phase 6 on our own SDL3 layer.
#include <stdafx.h>
#include "xam.h"
#include "xdm.h"
#include <cpu/guest_thread.h>
#include <ranges>
#include <unordered_set>
#include "xxHashMap.h"
#include <user/paths.h>
#include "overlapped.h"
#include <kernel/function.h>
#include <fstream>

namespace
{
    constexpr uint32_t XAM_ERROR_IO_PENDING = 0x3E5;
    constexpr uint32_t XAM_ERROR_ALREADY_EXISTS = 0xB7;
    constexpr uint32_t XAM_ERROR_FUNCTION_FAILED = 0x65B;

    // Asynchronous XAM calls (Xenia's CompleteOverlappedImmediate): with an
    // XOVERLAPPED the result goes there, its event is signalled, and the
    // call itself returns ERROR_IO_PENDING.
    uint32_t XamComplete(XXOVERLAPPED* overlapped, uint32_t result, uint32_t extended = 0, uint32_t length = 0)
    {
        if (!overlapped)
            return result;
        CompleteOverlapped(overlapped, result, extended, length);
        return XAM_ERROR_IO_PENDING;
    }
}

struct XamListener : KernelObject
{
    uint32_t id{};
    uint64_t areas{};
    std::vector<std::tuple<uint32_t, uint32_t>> notifications;

    XamListener(const XamListener&) = delete;
    XamListener& operator=(const XamListener&) = delete;

    XamListener();
    ~XamListener();
};

struct XamEnumeratorBase : KernelObject
{
    virtual uint32_t Next(void* buffer)
    {
        return -1;
    }
};

template<typename TIterator = std::vector<XHOSTCONTENT_DATA>::iterator>
struct XamEnumerator : XamEnumeratorBase
{
    uint32_t fetch;
    size_t size;
    TIterator position;
    TIterator begin;
    TIterator end;

    XamEnumerator() = default;
    XamEnumerator(uint32_t fetch, size_t size, TIterator begin, TIterator end) : fetch(fetch), size(size), position(begin), begin(begin), end(end)
    {

    }

    uint32_t Next(void* buffer) override
    {
        if (position == end)
        {
            return -1;
        }

        if (buffer == nullptr)
        {
            for (size_t i = 0; i < fetch; i++)
            {
                if (position == end)
                {
                    return i == 0 ? -1 : i;
                }

                ++position;
            }
        }

        for (size_t i = 0; i < fetch; i++)
        {
            if (position == end)
            {
                return i == 0 ? -1 : i;
            }

            memcpy(buffer, &*position, size);

            ++position;
            buffer = (void*)((size_t)buffer + size);
        }

        return fetch;
    }
};

std::array<xxHashMap<XHOSTCONTENT_DATA>, 3> gContentRegistry{};
static void ScanSavedContent();
std::unordered_set<XamListener*> gListeners{};
// Listeners and their queues: guest threads, and the main thread (the
// runtime's settings menu announces itself as system UI).
std::mutex gListenersMutex;
xxHashMap<std::string> gRootMap;

std::string_view XamGetRootPath(const std::string_view& root)
{
    const auto result = gRootMap.find(StringHash(root));

    if (result == gRootMap.end())
        return "";

    return result->second;
}

void XamRootCreate(const std::string_view& root, const std::string_view& path)
{
    gRootMap.emplace(StringHash(root), path);
}

XamListener::XamListener()
{
    std::lock_guard lock(gListenersMutex);
    gListeners.insert(this);
}

XamListener::~XamListener()
{
    std::lock_guard lock(gListenersMutex);
    gListeners.erase(this);
}

XCONTENT_DATA XamMakeContent(uint32_t type, const std::string_view& name)
{
    XCONTENT_DATA data{ 1, type };

    strncpy(data.szFileName, name.data(), sizeof(data.szFileName));

    return data;
}

void XamRegisterContent(const XCONTENT_DATA& data, const std::string_view& root)
{
    const auto idx = data.dwContentType - 1;

    gContentRegistry[idx].emplace(StringHash(data.szFileName), XHOSTCONTENT_DATA{ data }).first->second.szRoot = root;
}

void XamRegisterContent(uint32_t type, const std::string_view name, const std::string_view& root)
{
    XCONTENT_DATA data{ 1, type, {}, "" };

    strncpy(data.szFileName, name.data(), sizeof(data.szFileName));

    XamRegisterContent(data, root);
}

uint32_t XamNotifyCreateListener(uint64_t qwAreas)
{
    auto* listener = CreateKernelObject<XamListener>();

    listener->areas = qwAreas;

    return GetKernelHandle(listener);
}

void XamNotifyEnqueueEvent(uint32_t dwId, uint32_t dwParam)
{
    std::lock_guard lock(gListenersMutex);
    for (const auto& listener : gListeners)
    {
        if (((1 << MSG_AREA(dwId)) & listener->areas) == 0)
            continue;

        listener->notifications.emplace_back(dwId, dwParam);
    }
}

bool XNotifyGetNext(uint32_t hNotification, uint32_t dwMsgFilter, be<uint32_t>* pdwId, be<uint32_t>* pParam)
{
    auto& listener = *GetKernelObject<XamListener>(hNotification);
    std::lock_guard lock(gListenersMutex);

    if (dwMsgFilter)
    {
        for (size_t i = 0; i < listener.notifications.size(); i++)
        {
            if (std::get<0>(listener.notifications[i]) == dwMsgFilter)
            {
                if (pdwId)
                    *pdwId = std::get<0>(listener.notifications[i]);

                if (pParam)
                    *pParam = std::get<1>(listener.notifications[i]);

                listener.notifications.erase(listener.notifications.begin() + i);

                return true;
            }
        }
    }
    else
    {
        if (listener.notifications.empty())
            return false;

        if (pdwId)
            *pdwId = std::get<0>(listener.notifications[0]);

        if (pParam)
            *pParam = std::get<1>(listener.notifications[0]);

        listener.notifications.erase(listener.notifications.begin());

        return true;
    }

    return false;
}

uint32_t XamShowMessageBoxUI(uint32_t dwUserIndex, be<uint16_t>* wszTitle, be<uint16_t>* wszText, uint32_t cButtons,
    xpointer<be<uint16_t>>* pwszButtons, uint32_t dwFocusButton, uint32_t dwFlags, be<uint32_t>* pResult, XXOVERLAPPED* pOverlapped)
{
    *pResult = cButtons ? cButtons - 1 : 0;

#if _DEBUG
    assert("XamShowMessageBoxUI encountered!" && false);
#elif _WIN32
    // This code is Win32-only as it'll most likely crash, misbehave or
    // cause corruption due to using a different type of memory than what
    // wchar_t is on Linux. Windows uses 2 bytes while Linux uses 4 bytes.
    std::vector<std::wstring> texts{};

    texts.emplace_back(reinterpret_cast<wchar_t*>(wszTitle));
    texts.emplace_back(reinterpret_cast<wchar_t*>(wszText));

    for (size_t i = 0; i < cButtons; i++)
        texts.emplace_back(reinterpret_cast<wchar_t*>(pwszButtons[i].get()));

    for (auto& text : texts)
    {
        for (size_t i = 0; i < text.size(); i++)
            ByteSwapInplace(text[i]);
    }

    wprintf(L"[XamShowMessageBoxUI] !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!\n");
    wprintf(L"[XamShowMessageBoxUI] If you are encountering this message and the game has ceased functioning,\n");
    wprintf(L"[XamShowMessageBoxUI] please report it at https://github.com/SpeedBreakerProject/speedbreaker/issues.\n");
    wprintf(L"[XamShowMessageBoxUI] !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!\n");
    wprintf(L"[XamShowMessageBoxUI] %ls\n", texts[0].c_str());
    wprintf(L"[XamShowMessageBoxUI] %ls\n", texts[1].c_str());
    wprintf(L"[XamShowMessageBoxUI] ");

    for (size_t i = 0; i < cButtons; i++)
    {
        wprintf(L"%ls", texts[2 + i].c_str());

        if (i != cButtons - 1)
            wprintf(L" | ");
    }

    wprintf(L"\n");
    wprintf(L"[XamShowMessageBoxUI] Defaulted to button: %d\n", pResult->get());
    wprintf(L"[XamShowMessageBoxUI] !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!\n");
#endif

    XamNotifyEnqueueEvent(9, 0);

    return XamComplete(pOverlapped, ERROR_SUCCESS);
}

uint32_t XamContentCreateEnumerator(uint32_t dwUserIndex, uint32_t DeviceID, uint32_t dwContentType,
    uint32_t dwContentFlags, uint32_t cItem, be<uint32_t>* pcbBuffer, be<uint32_t>* phEnum)
{
    if (dwUserIndex != 0)
    {
        GuestThread::SetLastError(ERROR_NO_SUCH_USER);
        return 0xFFFFFFFF;
    }

    ScanSavedContent();
    const auto& registry = gContentRegistry[dwContentType - 1];
    const auto& values = registry | std::views::values;
    auto* enumerator = CreateKernelObject<XamEnumerator<decltype(values.begin())>>(cItem, sizeof(_XCONTENT_DATA), values.begin(), values.end());

    if (pcbBuffer)
        *pcbBuffer = sizeof(_XCONTENT_DATA) * cItem;

    *phEnum = GetKernelHandle(enumerator);

    return 0;
}

uint32_t XamEnumerate(uint32_t hEnum, uint32_t dwFlags, void* pvBuffer, uint32_t cbBuffer, be<uint32_t>* pcItemsReturned, XXOVERLAPPED* pOverlapped)
{
    auto* enumerator = GetKernelObject<XamEnumeratorBase>(hEnum);
    const auto count = enumerator->Next(pvBuffer);

    if (count == -1)
    {
        if (pcItemsReturned)
            *pcItemsReturned = 0;
        return pOverlapped ? XamComplete(pOverlapped, XAM_ERROR_FUNCTION_FAILED, 0x80070000 | ERROR_NO_MORE_FILES)
                           : ERROR_NO_MORE_FILES;
    }

    if (pcItemsReturned)
        *pcItemsReturned = count;

    return XamComplete(pOverlapped, ERROR_SUCCESS, 0, count);
}

// Content packages (saves) live in one directory each under the save path,
// with the package's XCONTENT_DATA beside it as <name>.xcontent so saves
// are found again (and enumerated with their display names) after a restart.
static std::filesystem::path ContentRoot(const XCONTENT_DATA& data)
{
    if (data.dwContentType == XCONTENTTYPE_SAVEDATA)
    {
        std::string name(data.szFileName, strnlen(data.szFileName, sizeof(data.szFileName)));
        for (char& c : name)
            if (c == '/' || c == '\\' || c == ':' || (c == '.' && &c == &name[0]))
                c = '_';
        return GetSavePath(true) / name;
    }
    if (data.dwContentType == XCONTENTTYPE_DLC)
        return GetGamePath() / "dlc";
    return GetGamePath();
}

static void ScanSavedContent()
{
    static bool scanned = false;
    if (scanned)
        return;
    scanned = true;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(GetSavePath(true), ec))
    {
        if (entry.path().extension() != ".xcontent")
            continue;
        XCONTENT_DATA data{};
        std::ifstream f(entry.path(), std::ios::binary);
        if (!f.read(reinterpret_cast<char*>(&data), sizeof(data)) || data.dwContentType != XCONTENTTYPE_SAVEDATA)
            continue;
        std::filesystem::path root = ContentRoot(data);
        if (!std::filesystem::is_directory(root, ec))
            continue;
        XamRegisterContent(data, (const char*)root.u8string().c_str());
        fprintf(stderr, "[xam] found save \"%.*s\"\n", int(strnlen(data.szFileName, sizeof(data.szFileName))), data.szFileName);
    }
}

uint32_t XamContentCreateEx(uint32_t dwUserIndex, const char* szRootName, const XCONTENT_DATA* pContentData,
    uint32_t dwContentFlags, be<uint32_t>* pdwDisposition, be<uint32_t>* pdwLicenseMask,
    uint32_t dwFileCacheSize, uint64_t uliContentSize, PXXOVERLAPPED pOverlapped)
{
    ScanSavedContent();
    auto& registry = gContentRegistry[pContentData->dwContentType - 1];
    const auto existing = registry.find(StringHash(pContentData->szFileName));
    const bool exists = existing != registry.end();
    const auto mode = dwContentFlags & 0xF;  // CREATE_NEW 1, CREATE_ALWAYS 2, OPEN_EXISTING 3, OPEN_ALWAYS 4, TRUNCATE_EXISTING 5

    if (pdwLicenseMask)
        *pdwLicenseMask = 0xFFFFFFFF;

    if (exists && mode == 1)
        return XamComplete(pOverlapped, XAM_ERROR_ALREADY_EXISTS);
    if (!exists && (mode == OPEN_EXISTING || mode == 5))
    {
        if (pdwDisposition)
            *pdwDisposition = XCONTENT_NEW;
        return XamComplete(pOverlapped, ERROR_PATH_NOT_FOUND);
    }
    if (mode < 1 || mode > 5)
        return XamComplete(pOverlapped, ERROR_PATH_NOT_FOUND);

    if (exists)
    {
        if (pdwDisposition)
            *pdwDisposition = XCONTENT_EXISTING;
        XamRootCreate(szRootName, existing->second.szRoot);
        return XamComplete(pOverlapped, ERROR_SUCCESS);
    }

    std::filesystem::path root = ContentRoot(*pContentData);
    std::error_code ec;
    std::filesystem::create_directories(root, ec);
    if (pContentData->dwContentType == XCONTENTTYPE_SAVEDATA)
    {
        std::filesystem::path meta = root;
        meta += ".xcontent";
        std::ofstream f(meta, std::ios::binary | std::ios::trunc);
        f.write(reinterpret_cast<const char*>(pContentData), sizeof(XCONTENT_DATA));
    }
    const std::string rootString = (const char*)root.u8string().c_str();
    XamRegisterContent(*pContentData, rootString);
    XamRootCreate(szRootName, rootString);
    if (pdwDisposition)
        *pdwDisposition = XCONTENT_NEW;
    return XamComplete(pOverlapped, ERROR_SUCCESS);
}

uint32_t XamContentClose(const char* szRootName, XXOVERLAPPED* pOverlapped)
{
    gRootMap.erase(StringHash(szRootName));
    return XamComplete(pOverlapped, ERROR_SUCCESS);
}

uint32_t XamContentGetDeviceData(uint32_t DeviceID, XDEVICE_DATA* pDeviceData)
{
    pDeviceData->DeviceID = DeviceID;
    pDeviceData->DeviceType = XCONTENTDEVICETYPE_HDD;
    pDeviceData->ulDeviceBytes = 0x10000000;
    pDeviceData->ulDeviceFreeBytes = 0x10000000;
    const char* name = "Hard Drive";
    for (size_t i = 0; i <= strlen(name); i++)
        pDeviceData->wszName[i] = uint16_t(name[i]);

    return 0;
}




// ---------------------------------------------------------------------------
// More content exports (SpeedBreaker; semantics from Xenia's xam_content.cc).
// The only device is the hard drive, id 1.

namespace
{
    constexpr uint32_t XAM_ERROR_FILE_NOT_FOUND = 0x2;
    constexpr uint32_t XAM_ERROR_INSUFFICIENT_BUFFER = 0x7A;
    constexpr uint32_t XAM_ERROR_DEVICE_NOT_CONNECTED = 0x48F;

    std::filesystem::path ThumbnailPath(const XCONTENT_DATA& data)
    {
        std::filesystem::path p = ContentRoot(data);
        p += ".thumbnail.png";
        return p;
    }
}

uint32_t XamContentGetDeviceName(uint32_t deviceId, be<uint16_t>* name, uint32_t capacity)
{
    if ((deviceId & 0xFFFF) != 1)
        return XAM_ERROR_DEVICE_NOT_CONNECTED;
    const char* text = "Hard Drive";
    size_t n = strlen(text);
    if (capacity < n + 1)
        return XAM_ERROR_INSUFFICIENT_BUFFER;
    for (size_t i = 0; i < n; i++)
        name[i] = uint16_t(text[i]);
    name[n] = 0;
    return ERROR_SUCCESS;
}

uint32_t XamContentCreate(uint32_t userIndex, const char* rootName, const XCONTENT_DATA* data, uint32_t flags,
    be<uint32_t>* disposition, be<uint32_t>* licenseMask, XXOVERLAPPED* overlapped)
{
    return XamContentCreateEx(userIndex, rootName, data, flags, disposition, licenseMask, 0, 0, overlapped);
}

uint32_t XamContentDelete(uint32_t userIndex, const XCONTENT_DATA* data, XXOVERLAPPED* overlapped)
{
    ScanSavedContent();
    auto& registry = gContentRegistry[data->dwContentType - 1];
    auto it = registry.find(StringHash(data->szFileName));
    if (it == registry.end())
        return XamComplete(overlapped, XAM_ERROR_FILE_NOT_FOUND);
    std::error_code ec;
    std::filesystem::path root = ContentRoot(*data);
    if (data->dwContentType == XCONTENTTYPE_SAVEDATA)
    {
        std::filesystem::remove_all(root, ec);
        std::filesystem::path meta = root;
        meta += ".xcontent";
        std::filesystem::remove(meta, ec);
        std::filesystem::remove(ThumbnailPath(*data), ec);
    }
    registry.erase(it);
    fprintf(stderr, "[xam] deleted content \"%.*s\"\n", int(strnlen(data->szFileName, sizeof(data->szFileName))), data->szFileName);
    return XamComplete(overlapped, ERROR_SUCCESS);
}

uint32_t XamContentFlush(const char* rootName, XXOVERLAPPED* overlapped)
{
    return XamComplete(overlapped, ERROR_SUCCESS);
}

uint32_t XamContentGetLicenseMask(be<uint32_t>* mask, XXOVERLAPPED* overlapped)
{
    if (mask)
        *mask = 0xFFFFFFFF;
    return XamComplete(overlapped, ERROR_SUCCESS);
}

uint32_t XamContentGetThumbnail(uint32_t userIndex, const XCONTENT_DATA* data, uint8_t* buffer, be<uint32_t>* bufferSize,
    XXOVERLAPPED* overlapped)
{
    std::ifstream f(ThumbnailPath(*data), std::ios::binary | std::ios::ate);
    if (!f)
        return XamComplete(overlapped, XAM_ERROR_FILE_NOT_FOUND);
    uint32_t size = uint32_t(f.tellg());
    uint32_t capacity = bufferSize ? uint32_t(*bufferSize) : 0;
    if (bufferSize)
        *bufferSize = size;
    if (!buffer || capacity < size)
        return XamComplete(overlapped, buffer ? XAM_ERROR_INSUFFICIENT_BUFFER : ERROR_SUCCESS);
    f.seekg(0);
    f.read(reinterpret_cast<char*>(buffer), size);
    return XamComplete(overlapped, ERROR_SUCCESS);
}

uint32_t XamContentSetThumbnail(uint32_t userIndex, const XCONTENT_DATA* data, const uint8_t* buffer, uint32_t size,
    XXOVERLAPPED* overlapped)
{
    std::ofstream f(ThumbnailPath(*data), std::ios::binary | std::ios::trunc);
    f.write(reinterpret_cast<const char*>(buffer), size);
    return XamComplete(overlapped, ERROR_SUCCESS);
}

// Voice: no headset, no voice objects (Xenia).
uint32_t XamVoiceHeadsetPresent(uint32_t voice) { return 0; }
uint32_t XamVoiceCreate(uint32_t unk1, uint32_t unk2, be<uint32_t>* out) { if (out) *out = 0; return 5; /* ERROR_ACCESS_DENIED */ }
uint32_t XamVoiceClose(uint32_t voice) { return 0; }
uint32_t XamVoiceSubmitPacket(uint32_t voice, uint32_t size, uint32_t data) { return 0; }

// Sign-in UI: user 0 is always signed in; show and close the "UI" at once.
uint32_t XamShowSigninUI(uint32_t paneCount, uint32_t flags)
{
    XamNotifyEnqueueEvent(9, 1);
    XamNotifyEnqueueEvent(9, 0);
    return ERROR_SUCCESS;
}

GUEST_FUNCTION_HOOK(__imp__XamContentGetDeviceName, XamContentGetDeviceName);
GUEST_FUNCTION_HOOK(__imp__XamContentCreate, XamContentCreate);
GUEST_FUNCTION_HOOK(__imp__XamContentDelete, XamContentDelete);
GUEST_FUNCTION_HOOK(__imp__XamContentFlush, XamContentFlush);
GUEST_FUNCTION_HOOK(__imp__XamContentGetLicenseMask, XamContentGetLicenseMask);
GUEST_FUNCTION_HOOK(__imp__XamContentGetThumbnail, XamContentGetThumbnail);
GUEST_FUNCTION_HOOK(__imp__XamContentSetThumbnail, XamContentSetThumbnail);
GUEST_FUNCTION_HOOK(__imp__XamVoiceHeadsetPresent, XamVoiceHeadsetPresent);
GUEST_FUNCTION_HOOK(__imp__XamVoiceCreate, XamVoiceCreate);
GUEST_FUNCTION_HOOK(__imp__XamVoiceClose, XamVoiceClose);
GUEST_FUNCTION_HOOK(__imp__XamVoiceSubmitPacket, XamVoiceSubmitPacket);
GUEST_FUNCTION_HOOK(__imp__XamShowSigninUI, XamShowSigninUI);
