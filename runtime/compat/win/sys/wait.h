#pragma once
#define WNOHANG 1
inline int waitpid(int, int*, int) { return -1; }
