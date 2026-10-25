// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING). See portable.h.
#include "portable.h"

#include <cerrno>
#include <cstdio>
#include <format>
#include <system_error>

namespace portable
{
    namespace
    {
        namespace fs = std::filesystem;

        // Normalized, no trailing separator.
        fs::path Normal(const fs::path& path)
        {
            fs::path p = path.lexically_normal();
            while (p.has_relative_path() && p.filename().empty())
                p = p.parent_path();
            return p;
        }

        // The record's text: UTF-8 everywhere (on Windows path.string() is
        // the code page's, UTF-8 only under the exe's manifest).
        std::string Utf8(const std::u8string& text)
        {
            return std::string(text.begin(), text.end());
        }

        // `relative` stays inside the folder it's relative to (not "", ".",
        // or "../...").
        bool Inside(const fs::path& relative)
        {
            return !relative.empty() && relative != "." && *relative.begin() != "..";
        }
    }

    Layout Resolve(const std::filesystem::path& root, bool marker, bool supported)
    {
        Layout layout;
        if (!supported || !marker || root.empty() || !root.is_absolute())
            return layout;
        layout.root = Normal(root);
        layout.data = layout.root / kDataFolder;
        layout.cache = layout.root / kCacheFolder;
        return layout;
    }

    bool MarkerIn(const std::filesystem::path& root)
    {
        std::error_code ec;
        return !root.empty() && fs::is_regular_file(root / kMarker, ec);
    }

    const Layout& Detect(const std::filesystem::path& root)
    {
        CurrentLayout() = Resolve(root, kSupported && MarkerIn(root));
        return Current();
    }

    std::string RecordText(const std::filesystem::path& folder, const Layout& layout)
    {
        fs::path path = Normal(folder);
        if (layout.Active())
            if (fs::path relative = path.lexically_relative(layout.root); Inside(relative))
                return Utf8(relative.generic_u8string());
        return Utf8(path.u8string());
    }

    std::optional<std::filesystem::path> ParseRecord(std::string_view text, const Layout& layout)
    {
        while (!text.empty() && (text.back() == '\n' || text.back() == '\r'))
            text.remove_suffix(1);
        if (text.empty())
            return std::nullopt;
        // UTF-8 (RecordText), whatever the system's code page is on Windows.
        fs::path path{ std::u8string(text.begin(), text.end()) };
        // (Records before v0.1.1 kept `--dest dir/`'s trailing separator.)
        if (path.is_absolute())
            return Normal(path);
        if (!layout.Active())
            return std::nullopt;
        fs::path relative = path.lexically_normal();
        if (!Inside(relative) || relative.has_root_path())
            return std::nullopt;
        return Normal(layout.root / relative);
    }

    int CheckWritable(const Layout& layout)
    {
        if (!layout.Active())
            return 0;
        std::error_code ec;
        fs::create_directories(layout.data, ec);
        if (ec)
        {
            std::error_condition condition = ec.default_error_condition();
            return condition.category() == std::generic_category() ? condition.value() : EIO;
        }
        // A real write: a read-only mount, a locked card or permissions all
        // show here, whatever the file system.
        fs::path probe = layout.data / ".write-check";
        errno = 0;
        FILE* f = fopen(probe.string().c_str(), "wb");
        if (!f)
            return errno ? errno : EACCES;
        bool ok = fputc('1', f) != EOF;
        int error = errno;
        ok = fclose(f) == 0 && ok;
        if (!ok && !error)
            error = errno;
        fs::remove(probe, ec);
        return ok ? 0 : (error ? error : EIO);
    }

    std::string Describe(const Layout& layout)
    {
        if (!layout.Active())
            return {};
        return std::format("[paths] portable mode ({} in {}): data in {}, caches in {}\n", kMarker, layout.root.string(),
            layout.data.string(), layout.cache.string());
    }
}
