// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING). See platform/folders.h.
#include <platform/folders.h>
#include <platform/os.h>

#include <knownfolders.h>
#include <shlobj.h>

namespace platform
{
    namespace
    {
        std::string Utf8(const wchar_t* wide)
        {
            int n = WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
            if (n <= 1)
                return {};
            std::string out(size_t(n - 1), '\0');
            WideCharToMultiByte(CP_UTF8, 0, wide, -1, out.data(), n, nullptr, nullptr);
            return out;
        }
    }

    std::filesystem::path GetKnownFolder(KnownFolder folder)
    {
        const KNOWNFOLDERID* id = nullptr;
        switch (folder)
        {
        case KnownFolder::LocalAppData: id = &FOLDERID_LocalAppData; break;
        case KnownFolder::Profile: id = &FOLDERID_Profile; break;
        case KnownFolder::Downloads: id = &FOLDERID_Downloads; break;
        case KnownFolder::Desktop: id = &FOLDERID_Desktop; break;
        case KnownFolder::Documents: id = &FOLDERID_Documents; break;
        }
        PWSTR path = nullptr;
        std::filesystem::path out;
        // KF_FLAG_DONT_VERIFY: just the location; nothing is created.
        if (SUCCEEDED(SHGetKnownFolderPath(*id, KF_FLAG_DONT_VERIFY, nullptr, &path)) && path)
            out = path;
        CoTaskMemFree(path);
        return out;
    }

    std::vector<Drive> Drives()
    {
        std::vector<Drive> drives;
        wchar_t systemDir[MAX_PATH] = L"";
        GetSystemDirectoryW(systemDir, MAX_PATH);
        DWORD mask = GetLogicalDrives();
        for (int i = 0; i < 26; i++)
        {
            if (!(mask & (1u << i)))
                continue;
            wchar_t root[4] = { wchar_t(L'A' + i), L':', L'\\', 0 };
            UINT type = GetDriveTypeW(root);
            if (type != DRIVE_FIXED && type != DRIVE_REMOVABLE)
                continue;
            wchar_t name[MAX_PATH + 1] = L"";
            DWORD flags = 0;
            // No medium in a card reader: no volume information; skip it.
            if (!GetVolumeInformationW(root, name, MAX_PATH + 1, nullptr, nullptr, &flags, nullptr, 0))
                continue;
            Drive d;
            d.root = root;
            d.label = name[0] ? Utf8(name) : Utf8(std::wstring(root, 2).c_str());
            d.removable = type == DRIVE_REMOVABLE;
            d.readOnly = (flags & FILE_READ_ONLY_VOLUME) != 0;
            d.system = towupper(systemDir[0]) == root[0];
            drives.push_back(std::move(d));
        }
        return drives;
    }

    bool IsCloudOnly(const std::filesystem::path& file)
    {
        DWORD attributes = GetFileAttributesW(file.c_str());
        return attributes != INVALID_FILE_ATTRIBUTES &&
            (attributes & (FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS | FILE_ATTRIBUTE_RECALL_ON_OPEN | FILE_ATTRIBUTE_OFFLINE)) != 0;
    }
}
