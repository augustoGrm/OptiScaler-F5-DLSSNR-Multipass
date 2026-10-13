// Host check of dlssnr/DlssNrEditCache.h, the edit cache's cadence: which frames run the model, what the passes are
// told (ring slice, frames back to the anchor, the crossfade), when it starts over and why it stands aside.
// cl /nologo /std:c++20 /EHsc /W4 tests\nr_edit_cache_smoke.cpp
#include "../OptiScaler/dlssnr/DlssNrEditCache.h"

#include <cmath>
#include <cstdio>
#include <string>

using namespace DlssNrEditCache;

static int fails = 0;
static void expect(bool ok, const std::string& what)
{
    if (!ok)
    {
        std::printf("FAIL: %s\n", what.c_str());
        ++fails;
    }
}

static Inputs On(unsigned long long clock, uint32_t interval = 4, uint32_t crossfade = 5)
{
    Inputs in;
    in.enabled = true;
    in.interval = interval;
    in.crossfade = crossfade;
    in.width = 2560;
    in.height = 1440;
    in.motionWidth = 1706;
    in.motionHeight = 960;
    in.clock = clock;
    in.revision = 7;
    return in;
}

int main()
{
    // Interval 4: R C C C R C C C, the ring slice steps every frame, cached frames trace back to their anchor
    {
        Cadence c;
        const char* pattern = "RCCCRCCCRC";
        for (int t = 0; t < 10; ++t)
        {
            const Step s = c.Next(On(100 + 2ull * t)); // frame generation: the clock steps by 2
            const bool refresh = pattern[t] == 'R';
            expect(s.runs && s.refresh == refresh && s.cached == !refresh, "interval 4 pattern at frame " + std::to_string(t));
            expect(s.ringSlice == (uint32_t) t % kRingSlices, "ring slice steps every frame (" + std::to_string(t) + ")");
            if (!refresh)
            {
                expect(s.traceSteps == (uint32_t) t % 4, "cached frame traces back to its anchor (" + std::to_string(t) + ")");
                expect(s.firstCarried == (t % 4 == 1), "first carried frame after the anchor");
            }
        }
    }

    // A refresh after cached frames hands the model the vectors back to its last run; the first one does not
    {
        Cadence c;
        Step s = c.Next(On(0));
        expect(s.refresh && !s.composeForModel, "the first refresh has nothing to compose");
        for (int t = 1; t < 4; ++t)
            c.Next(On(t));
        s = c.Next(On(4));
        expect(s.refresh && s.composeForModel && s.traceSteps == 4, "refresh composes 4 frames back to the last run");
    }

    // Crossfade 5 over interval 4: weights 1/5..4/5 on the 4 frames from a refresh, then the next refresh starts its own
    {
        Cadence c;
        Step s = c.Next(On(0));
        expect(s.fadeSteps == 0, "the first refresh does not fade (nothing was shown)");
        for (int t = 1; t < 4; ++t)
            c.Next(On(t));
        for (int k = 0; k < 4; ++k)
        {
            s = c.Next(On(4 + k));
            expect(s.fadeSteps == (uint32_t) k + 1 && std::abs(s.fadeWeight - (k + 1) / 5.0f) < 1e-6f,
                   "crossfade step " + std::to_string(k));
        }
        s = c.Next(On(8));
        expect(s.refresh && s.fadeSteps == 1, "the next refresh starts its own crossfade");
    }

    // Crossfade 3 over interval 8: weights 1/3, 2/3, then none
    {
        Cadence c;
        for (int t = 0; t < 8; ++t)
            c.Next(On(t, 8, 3));
        Step s = c.Next(On(8, 8, 3));
        expect(s.refresh && s.fadeSteps == 1, "crossfade 3: refresh frame");
        s = c.Next(On(9, 8, 3));
        expect(s.fadeSteps == 2 && std::abs(s.fadeWeight - 2.0f / 3.0f) < 1e-6f, "crossfade 3: second frame");
        s = c.Next(On(10, 8, 3));
        expect(s.fadeSteps == 0 && s.fadeWeight == 1.0f, "crossfade 3: done on the third frame");
    }

    // Crossfade 0 follows the interval: each N gets its measured length, never longer than N; set ones are held to 1..8
    {
        for (uint32_t n = kMinInterval; n <= kMaxInterval; ++n)
        {
            const uint32_t c = CrossfadeFor(n, 0);
            expect(c >= 1 && c <= n, "auto crossfade within 1..N at N=" + std::to_string(n));
            expect(c >= CrossfadeFor(n - 1 < kMinInterval ? kMinInterval : n - 1, 0), "auto crossfade grows with N");
        }
        expect(CrossfadeFor(4, 0) == kAutoCrossfade[4] && CrossfadeFor(1, 0) == kAutoCrossfade[2] &&
                   CrossfadeFor(99, 0) == kAutoCrossfade[8],
               "auto crossfade: the interval held to 2..8");
        expect(CrossfadeFor(4, 7) == 7 && CrossfadeFor(4, 99) == kMaxCrossfade, "a set crossfade is kept, held to 8");

        Cadence c;
        for (int t = 0; t < 4; ++t)
            c.Next(On(t, 4, 0));
        const uint32_t len = CrossfadeFor(4, 0);
        for (uint32_t k = 0; k + 1 < len; ++k)
        {
            const Step s = c.Next(On(4 + k, 4, 0));
            expect(s.fadeSteps == k + 1 && std::abs(s.fadeWeight - float(k + 1) / float(len)) < 1e-6f,
                   "auto crossfade at N=4, step " + std::to_string(k));
        }
        expect(c.Next(On(4 + len - 1, 4, 0)).fadeSteps == 0, "auto crossfade at N=4 done after its length");
    }

    // Starting over: a reset, a clock gap, a settings change, a resize -- a refresh with nothing to fade or compose
    {
        auto check = [](const char* what, auto change)
        {
            Cadence c;
            for (int t = 0; t < 2; ++t)
                c.Next(On(t));
            Inputs in = On(2);
            change(in);
            const Step s = c.Next(in);
            expect(s.refresh && s.fadeSteps == 0 && !s.composeForModel, std::string("starts over on ") + what);
        };
        check("a reset", [](Inputs& in) { in.reset = true; });
        check("a gap in the frame clock", [](Inputs& in) { in.clock = 50; });
        check("a clock that went back", [](Inputs& in) { in.clock = 0; });
        check("a settings change", [](Inputs& in) { in.revision = 8; });
        check("a resize", [](Inputs& in) { in.width = 1920; });
    }

    // A render-size change (dynamic resolution) goes on: the ring keeps uv displacements on a grid of its own
    {
        Cadence c;
        for (int t = 0; t < 2; ++t)
            c.Next(On(t));
        Inputs in = On(2);
        in.motionWidth = 1280;
        in.motionHeight = 720;
        const Step s = c.Next(in);
        expect(s.cached && s.traceSteps == 2, "a render-size change does not start over");
    }

    // Stands aside, and starts over when it comes back
    {
        Cadence c;
        c.Next(On(0));
        c.Next(On(1));
        Inputs in = On(2);
        in.blocked = true;
        Step s = c.Next(in);
        expect(!s.runs && s.standsAside != nullptr, "stands aside for Tune / frame hold / capture");
        s = c.Next(On(3));
        expect(s.runs && s.refresh && s.fadeSteps == 0, "comes back with a refresh");

        const auto reason = [](auto change)
        {
            Inputs in = On(0);
            change(in);
            return StandAsideReason(in) != nullptr;
        };
        expect(reason([](Inputs& in) { in.detailReuse = true; }), "aside: Reuse detail every other frame");
        expect(reason([](Inputs& in) { in.afterUpscale = false; }), "aside: before the upscaler / finished picture");
        expect(reason([](Inputs& in) { in.targetWritable = false; }), "aside: output not writable");
        expect(reason([](Inputs& in) { in.workAbove100 = true; }), "aside: model above 100%");
        expect(reason([](Inputs& in) { in.edgeCompression = true; }), "aside: Compress screen edges");
        expect(reason([](Inputs& in) { in.convertedEncoding = true; }), "aside: converted encoding");
        expect(reason([](Inputs& in) { in.debugOrCompare = true; }), "aside: debug view / Compare");
        expect(!reason([](Inputs&) {}), "runs by default");
    }

    // Off: nothing runs; the interval is held to 2..8
    {
        Cadence c;
        Inputs in = On(0);
        in.enabled = false;
        expect(!c.Next(in).runs, "off runs nothing");
        Cadence d;
        const char* pattern = "RCRCRC"; // interval 1 asked: held at 2
        for (int t = 0; t < 6; ++t)
            expect(d.Next(On(t, 1)).refresh == (pattern[t] == 'R'), "interval held at 2 (" + std::to_string(t) + ")");
        Cadence e;
        int refreshes = 0;
        for (int t = 0; t < 32; ++t)
            refreshes += e.Next(On(t, 99)).refresh ? 1 : 0;
        expect(refreshes == 4, "interval held at 8 (" + std::to_string(refreshes) + " refreshes in 32 frames)");
        expect(std::abs(e.TakeModelShare() - 4.0f / 32.0f) < 1e-6f, "the model's share of frames");
    }

    std::printf(fails == 0 ? "PASS: nr_edit_cache_smoke\n" : "%d FAILED\n", fails);
    return fails == 0 ? 0 : 1;
}
