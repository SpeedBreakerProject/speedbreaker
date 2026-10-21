#pragma once
typedef struct { const char* dli_fname; void* dli_fbase; const char* dli_sname; void* dli_saddr; } Dl_info;
inline int dladdr(const void*, Dl_info* i) { if (i) { i->dli_fname = nullptr; i->dli_fbase = nullptr; i->dli_sname = nullptr; i->dli_saddr = nullptr; } return 0; }
