#pragma once
#include <cstring>
struct utsname { char sysname[64], nodename[64], release[64], version[64], machine[64]; };
inline int uname(utsname* u) { std::memset(u, 0, sizeof(*u)); std::strcpy(u->sysname, "Windows"); std::strcpy(u->machine, "x86_64"); return 0; }
