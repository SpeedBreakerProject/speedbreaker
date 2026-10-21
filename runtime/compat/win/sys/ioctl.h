#pragma once
#define FIONREAD 0x541B
inline int ioctl(int, unsigned long, int*) { return -1; }
