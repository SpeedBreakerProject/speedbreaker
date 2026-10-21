#pragma once
#define RLIMIT_STACK 3
#define RLIM_INFINITY (~0ULL)
typedef unsigned long long rlim_t;
struct rlimit { rlim_t rlim_cur, rlim_max; };
inline int getrlimit(int, rlimit* l) { l->rlim_cur = 8ull << 20; l->rlim_max = RLIM_INFINITY; return 0; }
