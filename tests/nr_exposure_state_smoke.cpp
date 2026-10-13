// Host check of DlssNr_ExposureState.h: whether the NR Exposure controls act on the frame, for every colour encoding
// choice on the game-input path and on Finished Picture. No GPU and no game needed.
// cl /std:c++20 /EHsc tests/nr_exposure_state_smoke.cpp
#include "../OptiScaler/shaders/dlssnr/DlssNr_ExposureState.h"
#include "../OptiScaler/shaders/dlssnr/DlssNr_Common.h"
#include "../OptiScaler/shaders/dlssnr/DlssNr_LutConstants.h"

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

        // A long good run is not a licence: a run of bad reads starts the proof over (the share alone would need
        // ~50 bad reads after 1000 good ones)
        gate.Reset();
        for (int i = 0; i < 1000; ++i)
            gate.Count(true);
        for (int i = 0; i < 7; ++i)
            gate.Count(false);
        CHECK(gate.Trusted()); // 7 in a row: still trusted (1000 of 1007)
        gate.Count(true);
        for (int i = 0; i < 8; ++i)
            gate.Count(false);
        CHECK(!gate.Trusted() && gate.reads == 0); // 8 in a row: proven again from scratch
        for (int i = 0; i < 59; ++i)
            gate.Count(true);
        CHECK(!gate.Trusted());
        gate.Count(true);
        CHECK(gate.Trusted());
    }

    // The LUT pass gets the encode's own white point fields (CopyLiveWhitePoint): each field to its own, the anchors as
    // key/trim pairs in order. A swapped field would grade for another white with nothing else to show it.
    {
        DlssNrConstants encode {};
        encode.WhitePoint = 2.5f;
        encode.PreExposure = 0.75f;
        encode.UseGameExposure = 1u;
        encode.UseExposureWhitePoint = 2u;
        encode.ExposureBaseScale = 0.5f;
        encode.ExposureTrim = 1.25f;
        encode.ExposureTrimPreview = 3u;
        encode.ExposureTrimAnchorCount = 8u;
        float* pairs = &encode.ExposureTrimAnchorExposure0;
        for (int i = 0; i < 16; ++i)
            pairs[i] = 10.0f + (float) i;
        encode.ExposureTrimAnchorTrim7 = 99.0f;

        DlssNrLutConstants lut {};
        CopyLiveWhitePoint(lut, encode);
        CHECK(lut.Trim == 2.5f && lut.PreExposure == 0.75f && lut.UseGameExposure == 1u);
        CHECK(lut.UseExposureWhitePoint == 2u && lut.ExposureBaseScale == 0.5f && lut.ExposureTrim == 1.25f);
        CHECK(lut.ExposureTrimPreview == 3u && lut.ExposureTrimAnchorCount == 8u);
        CHECK(lut.ExposureTrimAnchors[0] == 10.0f && lut.ExposureTrimAnchors[1] == 11.0f);
        CHECK(lut.ExposureTrimAnchors[14] == 24.0f && lut.ExposureTrimAnchors[15] == 99.0f); // Trim7 is the last
    }

    printf(fails == 0 ? "PASS\n" : "%d FAILED\n", fails);
    return fails == 0 ? 0 : 1;
}
