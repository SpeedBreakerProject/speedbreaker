#pragma once
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#define ST_RDONLY 1
struct statvfs { uint64_t f_bavail, f_frsize, f_flag; };
inline int statvfs(const char* p, struct statvfs* v)
{
    std::error_code ec;
    auto s = std::filesystem::space(std::filesystem::path(p), ec);
    if (ec) return -1;
    v->f_frsize = 1; v->f_bavail = s.available; v->f_flag = 0;
    return 0;
}
inline int statvfs(const wchar_t* p, struct statvfs* v)
{
    std::error_code ec;
    auto s = std::filesystem::space(std::filesystem::path(p), ec);
    if (ec) return -1;
    v->f_frsize = 1; v->f_bavail = s.available; v->f_flag = 0;
    return 0;
}
