// Host check of DlssNr_ExposureState.h: whether the NR Exposure controls act on the frame, for every colour encoding
// choice on the game-input path and on Finished Picture. No GPU and no game needed.
// cl /std:c++20 /EHsc tests/nr_exposure_state_smoke.cpp
#include "../OptiScaler/shaders/dlssnr/DlssNr_ExposureState.h"

#include <cmath>
#include <cstdio>

using namespace DlssNrColourEncoding;
using DlssNrExposureState::State;

static int fails = 0;
#define CHECK(c)                                                                                                       \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(c))                                                                                                      \
        {                                                                                                              \
            printf("FAIL line %d: %s\n", __LINE__, #c);                                                                \
            ++fails;                                                                                                   \
        }                                                                                                              \
    } while (0)

int main()
{
    using DlssNrExposureState::For;

    // Game-input path (a DLSS evaluate): Auto follows the HDR flag and the format.
    CHECK(For(Resolve(kAuto, true, true), false) == State::LinearHdr);
    CHECK(For(Resolve(kAuto, true, false), false) == State::ToneMapped);
    CHECK(For(Resolve(kAuto, false, true), false) == State::ToneMapped);

    // Forced choices on the game-input path: sRGB and gamma 2.2 pass through, PQ is display light.
    CHECK(For(Resolve((uint32_t) Encoding::Srgb, true, true), false) == State::ToneMapped);
    CHECK(For(Resolve((uint32_t) Encoding::Gamma22, false, false), false) == State::ToneMapped);
    CHECK(For(Resolve((uint32_t) Encoding::Pq, false, false), false) == State::DisplayPinned);
    CHECK(For(Resolve((uint32_t) Encoding::LinearHdr, false, true), false) == State::LinearHdr);

    // Finished Picture (the screen): SDR passes through, scRGB and HDR10 are display light.
    CHECK(For(ResolveFinished(kAuto, true, Screen::Sdr, false, true).choice, true) == State::ToneMapped);
    CHECK(For(ResolveFinished(kAuto, true, Screen::Scrgb, true, false).choice, true) == State::DisplayPinned);
    CHECK(For(ResolveFinished(kAuto, true, Screen::Pq, false, true).choice, true) == State::DisplayPinned);
    CHECK(For(ResolveFinished((uint32_t) Encoding::Gamma22, true, Screen::Sdr, false, true).choice, true) ==
          State::ToneMapped);

    CHECK(DlssNrExposureState::kReferenceWhiteNits == 203.0f);

    // The display paper white: the default gives exactly the white points the pass always pinned.
    {
        using namespace DlssNrExposureState;
        CHECK(PinnedWhitePoint(203.0f, kNitsPerPqDecodedUnit) == 1.0f);
        CHECK(PinnedWhitePoint(203.0f, kNitsPerScrgbUnit) == 203.0f / 80.0f);
        CHECK(PinnedWhitePoint(406.0f, kNitsPerPqDecodedUnit) == 2.0f);
        CHECK(PinnedWhitePoint(400.0f, kNitsPerScrgbUnit) == 5.0f);
        // Bounded at the point of use; not a number is the default.
        CHECK(PaperWhiteNits(10.0f) == kMinPaperWhiteNits);
        CHECK(PaperWhiteNits(5000.0f) == kMaxPaperWhiteNits);
        CHECK(PaperWhiteNits(std::nanf("")) == kReferenceWhiteNits);
        CHECK(PaperWhiteNits(-INFINITY) == kReferenceWhiteNits);
        CHECK(PaperWhiteNits(250.0f) == 250.0f);
    }

    // Vulkan's live Game exposure gate: 60 reads, 95% believable.
    {
        DlssNrExposureState::LiveGameGate gate;
        CHECK(!gate.Trusted());
        for (int i = 0; i < 59; ++i)
            gate.Count(true);
        CHECK(!gate.Trusted()); // 59 reads
        gate.Count(true);
        CHECK(gate.Trusted()); // 60 of 60
        for (int i = 0; i < 3; ++i)
            gate.Count(false);
        CHECK(gate.Trusted()); // 60 of 63 = 95.2%
        gate.Count(false);
        CHECK(!gate.Trusted()); // 60 of 64 = 93.8%
        gate.Reset();
        CHECK(!gate.Trusted() && gate.reads == 0);
        for (int i = 0; i < 100; ++i)
            gate.Count(i % 2 == 0);
        CHECK(!gate.Trusted()); // a layout guess that half holds never goes live
    }

    printf(fails == 0 ? "PASS\n" : "%d FAILED\n", fails);
    return fails == 0 ? 0 : 1;
}
