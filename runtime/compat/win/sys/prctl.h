#pragma once
#define PR_SET_TIMERSLACK 29
#define PR_SET_PTRACER 0x59616d61
#define PR_SET_PTRACER_ANY (~0UL)
inline int prctl(int, ...) { return 0; }
