#pragma once
#include <cstdlib>
struct passwd { const char* pw_dir; const char* pw_name; };
inline passwd* getpwuid(int) { static passwd p; p.pw_dir = std::getenv("USERPROFILE"); p.pw_name = std::getenv("USERNAME"); return p.pw_dir ? &p : nullptr; }
inline int getuid() { return 0; }
