// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING).
//
// Two diagnostics for the sun shadow map (GitHub #90). Neither is a setting:
// unset, nothing changes.
//
// NFSMW_SHADOW_SCALE=<sx>x<sy> (or <s>): the shadow map's scale per axis,
// whatever Shadow Resolution says (gpu/render_scale.h ParseRenderScale's
// spellings: 1, 1.5, 2, 3, 4 or a ratio from 1 to 4 on each axis, so "2x1",
// "9/4x1", "3x1"). The map is 1600 x 1600 guest texels; x is the map's u,
// which the game's fit lays across the view (game/ultrawide.cpp), y its v,
// the view's depth. "2x1" is a 3200 x 1600 map.
//
// NFSMW_SHADOW_AB: the sun shadow's variants in turn in ONE run, so they can
// be compared on the same scenes (pictures taken mid-phase, GPU time per
// variant on the same route):
//
//   NFSMW_SHADOW_AB=<seconds>[@<from>]:<variant>,<variant>,...
//
// Each phase lasts <seconds> (from the first frame, the clock the
// NFSMW_CHECK_PRESENT_SEC captures use); variant k runs in phases k, k + N,
// k + 2N... A variant is '.'-joined options, unset ones as the run's own:
// smooth / classic (Shadow Edges), fit=view|viewh|aspect|game
// (game/ultrawide.cpp's NFSMW_SHADOW_FIT; game is v0.2.1's fit), and the
// map's scale: 1x / 1.5x / 2x (both axes, as Shadow Resolution's) or
// map=<sx>x<sy> (NFSMW_SHADOW_SCALE's spellings). A '.' between two digits
// belongs to a number ("1.5x", "map=1.5x1"), so an option that starts with a
// digit doesn't follow one that ends with one.
// Example: 3@150:1x,map=2x1,map=9/4x1,2x,1x.fit=game,map=3x1.classic.
// A [shadowab] line names each phase's variant as it starts, and, from
// <from> seconds on, every N phases the GPU executing time per frame of each
// variant so far (its phases' frames after their first half second, so a
// resolution change's rebuild isn't counted; NFSMW_GPU_TIMING=1 for those).
#pragma once
#include "render_scale.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

namespace gpu::shadow_ab
{
    // The map's guest size on each axis, and the largest host one a forced
    // scale may give (4x).
    constexpr uint32_t kMapTexels = 1600;

    // A forced scale from `text` (NFSMW_SHADOW_SCALE's value, or a map=
    // option's): ParseRenderScale's spellings. False for anything else.
    inline bool ParseMapScale(std::string_view text, RenderScale& out)
    {
        const std::string s(text);
        RenderScale parsed;
        if (!ParseRenderScale(s.c_str(), parsed))
            return false;
        out = parsed;
        return true;
    }

    // The map's host size at `s`.
    inline uint32_t MapWidth(const RenderScale& s) { return s.x.Size(kMapTexels); }
    inline uint32_t MapHeight(const RenderScale& s) { return s.y.Size(kMapTexels); }

    // The map's host texels at `s` against the 1x map's (4 at 2x, 2 at 2x1,
    // 2.25 at 9/4x1 and at 1.5x).
    inline double TexelRatio(const RenderScale& s)
    {
        return double(MapWidth(s)) * double(MapHeight(s)) / (double(kMapTexels) * double(kMapTexels));
    }

    struct Variant
    {
        std::string name;
        int smooth = -1;         // Shadow Edges: -1 the run's, 0 Classic, 1 Smooth
        int fit = -1;            // -1 the run's; FitGame..FitViewH
        bool hasScale = false;   // false: the run's (Shadow Resolution, NFSMW_SHADOW_SCALE)
        RenderScale scale;
    };
    enum Fit : int { FitGame = 0, FitAspect = 1, FitView = 2, FitViewH = 3 };

    inline bool ParseVariant(std::string_view s, Variant& v)
    {
        v = Variant{};
        v.name = std::string(s);
        const auto digit = [](char c) { return c >= '0' && c <= '9'; };
        while (!s.empty())
        {
            // The next '.' that isn't inside a number.
            size_t dot = 0;
            for (;; dot++)
            {
                dot = s.find('.', dot);
                if (dot == std::string_view::npos)
                    break;
                if (!(dot > 0 && dot + 1 < s.size() && digit(s[dot - 1]) && digit(s[dot + 1])))
                    break;
            }
            const std::string_view t = s.substr(0, dot);
            s = dot == std::string_view::npos ? std::string_view() : s.substr(dot + 1);
            if (t == "smooth" || t == "pcf") v.smooth = 1;
            else if (t == "classic" || t == "nopcf") v.smooth = 0;
            else if (t == "fit=game") v.fit = FitGame;
            else if (t == "fit=aspect") v.fit = FitAspect;
            else if (t == "fit=view") v.fit = FitView;
            else if (t == "fit=viewh") v.fit = FitViewH;
            else if (t == "1x" || t == "1.5x" || t == "2x")
            {
                v.hasScale = true;
                v.scale = ShadowScale(t == "1x" ? 2u : t == "1.5x" ? 3u : 4u);
            }
            else if (t.substr(0, 4) == "map=")
            {
                if (!ParseMapScale(t.substr(4), v.scale))
                    return false;
                v.hasScale = true;
            }
            else
                return false;
        }
        return true;
    }

    struct Plan
    {
        double period = 0.0;  // 0: off
        double from = 0.0;
        std::vector<Variant> variants;
    };

    inline bool ParsePlan(const char* text, Plan& plan)
    {
        plan = Plan{};
        if (!text || !*text)
            return false;
        std::string_view s = text;
        size_t colon = s.find(':');
        if (colon == std::string_view::npos)
            return false;
        std::string head(s.substr(0, colon));
        size_t at = head.find('@');
        plan.period = std::strtod(head.substr(0, at).c_str(), nullptr);
        if (at != std::string::npos)
            plan.from = std::strtod(head.substr(at + 1).c_str(), nullptr);
        if (!(plan.period > 0.0))
            return false;
        for (std::string_view rest = s.substr(colon + 1); !rest.empty();)
        {
            size_t comma = rest.find(',');
            Variant v;
            if (!ParseVariant(rest.substr(0, comma), v))
                return false;
            plan.variants.push_back(v);
            rest = comma == std::string_view::npos ? std::string_view() : rest.substr(comma + 1);
        }
        return !plan.variants.empty();
    }

    // The plan from the environment (parsed once; period 0 when unset or malformed).
    inline const Plan& EnvPlan()
    {
        static const Plan plan = [] {
            Plan p;
            const char* text = std::getenv("NFSMW_SHADOW_AB");
            if (!ParsePlan(text, p))
            {
                if (text && *text)
                    fprintf(stderr, "[shadowab] NFSMW_SHADOW_AB=%s ignored: <seconds>[@<from>]:<variant>,... (gpu/shadow_ab.h)\n", text);
                p = Plan{};
            }
            return p;
        }();
        return plan;
    }

    // NFSMW_SHADOW_SCALE (parsed once): the forced scale, or null when unset
    // or malformed.
    inline const RenderScale* EnvScale()
    {
        struct Forced { bool on = false; RenderScale scale; };
        static const Forced forced = [] {
            Forced f;
            if (const char* text = std::getenv("NFSMW_SHADOW_SCALE"))
            {
                f.on = ParseMapScale(text, f.scale);
                if (f.on)
                    fprintf(stderr, "[renderer] NFSMW_SHADOW_SCALE=%s: the sun shadow map %ux%u, whatever Shadow Resolution says\n", text,
                        MapWidth(f.scale), MapHeight(f.scale));
                else
                    fprintf(stderr, "[renderer] NFSMW_SHADOW_SCALE=%s ignored: <sx>x<sy>, each 1, 1.5, 2, 3 or 4 or a ratio (\"2x1\", \"9/4x1\")\n", text);
            }
            return f;
        }();
        return forced.on ? &forced.scale : nullptr;
    }

    // The variant in force for the guest's shadow fit (game/ultrawide.cpp):
    // FitGame..FitViewH, or -1 (the run's NFSMW_SHADOW_FIT). Set by the renderer.
    inline std::atomic<int> g_fit{ -1 };
}
