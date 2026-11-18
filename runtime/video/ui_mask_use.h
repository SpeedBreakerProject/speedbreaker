// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING).
//
// Sharper UI's UI mask (UI-C: gpu/sharp_ui.h kUiMaskStoreGlsl, gpu/renderer.cpp
// UiMaskState, video/presenter.cpp's UI pass) only feeds the presenter's UI
// pass, and that pass draws only over a picture the bicubic scale pass
// enlarges by a non-integer factor (2x2's 2560x1440 at 4K: x1.5), without
// HDR. Where it doesn't (the Mac's first window: 2560x1440 at Auto 2x2, shown
// 1:1), the mask was built every frame and never read: each front-end
// fragment's atomic, the 3x3 resolve over the whole frame, its clear.
//
// NFSMW_UI_MASK_LAZY=1 (the default; 0: as before): the presenter says, for
// each picture it draws, whether its UI pass would apply to a picture of
// that size (the pass's own rule, PassApplies); the renderer, at each real
// front image, keeps building the mask for the next one only when the last
// word for that size is "applies" or there is no word for it yet (Useful).
// The front-end draws keep their pipelines (the same mask-writing variant);
// a skipped frame's draws write a 1x1 image instead (gpu/renderer.cpp), so
// every picture is drawn by the same shaders as before. Pure functions, for
// tests/ui_mask_use_test.cpp.
#pragma once
#include <cstdint>

#include "upscale.h"

namespace video::ui_mask
{
    // The presenter's UI pass applies to a w x h picture shown at dw x dh:
    // the pass on (Sharper UI on, NFSMW_UI_MASK not 0), the bicubic scale
    // pass, no HDR swapchain, the picture enlarged, and not by a whole
    // factor on both axes (an integer enlargement keeps texels whole).
    inline bool PassApplies(bool on, bool bicubic, bool hdr, uint32_t w, uint32_t h, int32_t dw, int32_t dh)
    {
        if (!on || !bicubic || hdr || !upscale::Enlarged(w, h, dw, dh))
            return false;
        return uint32_t(dw) % w != 0 || uint32_t(dh) % h != 0;
    }

    // The presenter's last word, one 64-bit atomic: bit 63 set once it has
    // said anything; bits 32-62 the picture's width, 1-31 its height, bit 0
    // whether the pass applied.
    inline uint64_t Pack(uint32_t w, uint32_t h, bool applies)
    {
        return (1ull << 63) | (uint64_t(w & 0x7FFFFFFFu) << 32) | (uint64_t(h & 0x7FFFFFFFu) << 1) | (applies ? 1ull : 0ull);
    }

    // Whether to build the mask for the next w x h front image: unless the
    // presenter has said the pass doesn't apply to a picture of exactly
    // that size (nothing said yet, or another size: build it, as before).
    inline bool Useful(uint64_t word, uint32_t w, uint32_t h)
    {
        if (!(word >> 63))
            return true;
        const uint32_t ww = uint32_t(word >> 32) & 0x7FFFFFFFu, wh = uint32_t(word >> 1) & 0x7FFFFFFFu;
        if (ww != (w & 0x7FFFFFFFu) || wh != (h & 0x7FFFFFFFu))
            return true;
        return (word & 1) != 0;
    }
}
