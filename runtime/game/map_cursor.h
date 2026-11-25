// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING).
//
// The World Map's cursor at the Xbox 360's speed at any frame rate
// (game/map_cursor.cpp hooks the game; tests/map_cursor_test.cpp checks this
// header alone). GitHub issue #14 (telmotrooper's report there).
//
// The World Map (WorldMapMain.fng) opens from free roam with the D-pad's left
// (the in-game action 47 in sub_822A9798 fires EWorldMapOn, sub_82398A10);
// the right stick moves its cursor ("(R) Move Cursor"). The screen (factory
// sub_822D4AC0, 376 bytes, constructor sub_82252148) moves the cursor on the
// front end's per-frame message 0xC98356BA, which sub_82285F60 sends once a
// frame: the frame (sub_823AFE20, once per main loop iteration) calls it at
// 0x823AFF88, unconditionally, whatever the world steps. The screen's
// handler (sub_822523F0) then runs the cursor step (sub_82253248):
//   - it drains the screen's action queue (this+292, sub_822530A8): the
//     stick's actions 29/30 set the y velocity at this+300 to the action's
//     value times -14/+14, 31/32 the x velocity at this+296 (the constants at
//     0x820B0250 and 0x820AFDC8, front end units);
//   - when the map isn't zooming and the velocity isn't (0, 0), it calls
//     MoveCursor (sub_822534F0; f1 = x, f2 = y; called from 0x82253408 only):
//     the cursor object (this+288) moves by (x, y), and past the map's edge
//     the map (this+352) scrolls by the rest instead.
// So the cursor moves the same step every frame. The 360 runs the game at 30
// fps; at SpeedBreaker's 60 there are twice the frames, and the cursor went
// twice as far a second.
//
// The fix keeps the game's code and its velocity, and scales the step it
// hands MoveCursor by the game time of the frame over the 360's frame (2/60
// s), at most 1: game::CurrentFrameMs, the whole vblanks the main loop gives
// the frame (game/frame_time.h). At Frame Rate 30 a frame is 2/60 s: the
// game's step, nothing changes. At 60 a frame is 1/60 s: half the step, 60
// times a second, so the speed a second is the 360's and the cursor still
// moves every frame (as smooth as the picture). A slow frame of 2 vblanks or
// more takes the whole step, as the 360's frame does (the 360 doesn't make up
// for a frame that took 3 vblanks either). Smooth Motion's in-between pictures
// don't run the frame (game/hfr.cpp draws them from sub_823AFC68), so they
// don't step the cursor.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace game::map_cursor
{
    // ctx.lr when the cursor step (sub_82253248) calls MoveCursor
    // (sub_822534F0) with the screen's velocity (bl at 0x82253408).
    constexpr uint32_t kStepReturn = 0x8225340C;
    // The WorldMap screen's velocity, front end units a frame (this = r3).
    constexpr uint32_t kVelocityX = 296, kVelocityY = 300;
    // Its cursor object (an FEObject; for the log only: its position at
    // [obj + 44] + 28 / + 32, where sub_82280D88 reads it for every object
    // type but a string's, 2).
    constexpr uint32_t kCursorObject = 288;
    constexpr uint32_t kObjectType = 24, kObjectData = 44, kDataX = 28, kDataY = 32, kStringType = 2;
    // The game's step at full deflection, front end units a frame.
    constexpr float kGameStep = 14.0f;

    // One vblank of game time, a world step; the 360's frame is two.
    constexpr double kVblankMs = 1000.0 / 60.0;
    constexpr double kConsoleFrameMs = 2.0 * kVblankMs;
    // A frame time this close to whole vblanks is those vblanks: the main loop
    // hands it in 16.16 ms (2/60 s arrives as 33.3333282 ms), and Frame Rate
    // 30 must keep the game's step to the bit.
    constexpr double kSnapMs = 0.01;

    // The share of the game's step a frame of `frameMs` of game time moves the
    // cursor: frameMs over the 360's frame, at most the whole step. No frame
    // time (0, negative, NaN: none handed yet) is the game's step.
    inline double StepScale(double frameMs)
    {
        if (!(frameMs > 0.0))
            return 1.0;
        const double vblanks = std::round(frameMs / kVblankMs);
        if (vblanks >= 1.0 && std::abs(frameMs - vblanks * kVblankMs) <= kSnapMs)
            frameMs = vblanks * kVblankMs;
        return std::min(frameMs / kConsoleFrameMs, 1.0);
    }
}
