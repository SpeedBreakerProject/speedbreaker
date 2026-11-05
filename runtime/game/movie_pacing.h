// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING). See movie_pacing.cpp.
#pragma once
#include <cstdint>

namespace game
{
    // Any thread: when the XMV player last rendered a movie frame (ns, steady
    // clock; 0: none yet). A movie (the boot logos, the intro, a cutscene)
    // plays while these keep coming.
    int64_t LastMovieFrameNs();

    // Any thread: XMV frames rendered so far (NFSMW_CHECK_MOVIE_FRAMES captures by it).
    uint64_t MovieFramesRendered();
}
