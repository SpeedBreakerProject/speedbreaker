// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING).
#pragma once
#include <ppc_config.h>
#include <ppc_context.h>

// Called by every import stub that has no implementation yet. Logs the import,
// its first argument registers and the guest return address, then halts
// (exit code 3). NFSMW_STUB_CONTINUE=1 returns 0 to the game instead, for
// exploring further; every call is still logged.
void UnimplementedImport(const char* name, PPCContext& ctx, uint8_t* base);
