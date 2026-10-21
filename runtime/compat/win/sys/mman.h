// SpeedBreaker Windows port: the little of <sys/mman.h> and sysconf the
// runtime uses (implemented in compat/win/win_compat.cpp). GPL-3.0-or-later.
#pragma once
#include <cstddef>
#define PROT_NONE 0
#define PROT_READ 1
#define PROT_WRITE 2
#define _SC_PAGESIZE 30
long sysconf(int name);
int mprotect(void* addr, size_t len, int prot);
