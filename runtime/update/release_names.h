// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING).
//
// The names the update check spells for this build's game: its GitHub
// repository (SB_GITHUB_REPO) and the prefix of its release files
// (SB_ASSET_PREFIX: "SpeedBreaker-macos.dmg", "SpeedBreaker-Carbon-macos.dmg").
// The engine is shared by both games, so these come from game/game_info.h
// where a build has it (NFS: Carbon's tree), and otherwise are Most Wanted's,
// which this tree builds. A game_info.h that defines them wins; one that
// doesn't define SB_ASSET_PREFIX yet gets it from SB_GAME_CARBON here.
// Literals, so they join others at compile time.
#pragma once

#if __has_include(<game/game_info.h>)
#include <game/game_info.h>
#endif

#ifndef SB_GITHUB_REPO
#ifdef SB_GAME_CARBON
#define SB_GITHUB_REPO "SpeedBreakerProject/speedbreaker-carbon"
#else
#define SB_GITHUB_REPO "SpeedBreakerProject/speedbreaker"
#endif
#endif

#ifndef SB_ASSET_PREFIX
#ifdef SB_GAME_CARBON
#define SB_ASSET_PREFIX "SpeedBreaker-Carbon"
#else
#define SB_ASSET_PREFIX "SpeedBreaker"
#endif
#endif

#ifndef SB_APP_NAME
#ifdef SB_GAME_CARBON
#define SB_APP_NAME "SpeedBreaker: Carbon"
#else
#define SB_APP_NAME "SpeedBreaker"
#endif
#endif
