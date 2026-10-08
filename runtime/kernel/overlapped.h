// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING).
#pragma once
#include <cstdint>
#include <xbox.h>

// Completes an XOVERLAPPED like Xenia's KernelState::CompleteOverlappedEx:
// result, extended error and length, then signals its event. Completion
// routines (APCs) are not supported yet and assert.
void CompleteOverlapped(XXOVERLAPPED* overlapped, uint32_t result, uint32_t extendedError = 0, uint32_t length = 0);
