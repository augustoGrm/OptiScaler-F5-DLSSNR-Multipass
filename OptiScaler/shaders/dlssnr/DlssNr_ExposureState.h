#pragma once

// Whether the NR Exposure controls (white point source, Trim, Automatic, Paper white, Tune) decide how bright the
// picture handed to the model is, on the frame the pass last ran on. Three cases:
//
//   LinearHdr      the frame is open-ended linear light; the white point the controls pick divides it before the
//                  proxy curve. The only case the controls act in.
//   ToneMapped     the game already tone mapped the frame (an SDR picture): it reaches the model as it is, with no
//                  white point at all (dlssnr.hlsl's passthrough). Paper white then only scales the debug views.
//   DisplayPinned  display-referred HDR -- a forced PQ frame on the game-input path, or Finished Picture on an scRGB or
//                  HDR10 screen. White is the game's HDR paper white ([DlssNr] DisplayPaperWhiteNits, ITU-R BT.2408's
//                  203 nits by default), not an exposure, so the controls are skipped.
//
// Decided from the colour encoding choice (DlssNr_ColourEncoding.h) alone, so the menu can say which case it is
// without asking the backend. Header-only and free of D3D and Vulkan types (tests/nr_exposure_state_smoke.cpp).

#include "DlssNr_ColourEncoding.h"

#include <cmath>
#include <cstdint>

namespace DlssNrExposureState
{
enum class State : uint32_t
{
    LinearHdr,
    ToneMapped,
    DisplayPinned,
};

// ITU-R BT.2408's reference white, the default white of a display-referred HDR frame.
constexpr float kReferenceWhiteNits = 203.0f;

// The game's HDR paper white is a player setting (often 100-400 nits; a game's UI may sit at the Windows SDR level), so
// [DlssNr] DisplayPaperWhiteNits sets it. Bounded here, at the point of use: an ini value outside the range, or not a
// number, gives the nearest bound or the default.
constexpr float kMinPaperWhiteNits = 80.0f;
constexpr float kMaxPaperWhiteNits = 1000.0f;

inline float PaperWhiteNits(float setting)
{
    if (!std::isfinite(setting))
        return kReferenceWhiteNits;
    return setting < kMinPaperWhiteNits   ? kMinPaperWhiteNits
           : setting > kMaxPaperWhiteNits ? kMaxPaperWhiteNits
                                          : setting;
}

// Nits in one unit of the light the pass reads a display-referred frame in: the PQ decode puts the reference white
// (203 nits) at 1.0 (dlssnr.hlsl PqToReferenceLinear); scRGB is 80 nits per unit (Finished Picture's PQ conversion
// writes scRGB too).
constexpr float kNitsPerPqDecodedUnit = 203.0f;
constexpr float kNitsPerScrgbUnit = 80.0f;

// The white point of a display-referred frame. At the default this is exactly the value the pass always pinned
// (1.0 after the PQ decode, 203/80 in scRGB).
inline float PinnedWhitePoint(float paperWhiteNitsSetting, float nitsPerUnit)
{
    return PaperWhiteNits(paperWhiteNitsSetting) / nitsPerUnit;
}

// Vulkan only: when the game's own exposure image may be read live in the shader, as D3D12 reads it. The layout the
// game leaves that image in is not known on Vulkan, so the host first reads it a few frames behind (the meter's
// courier, through the same layout guess) and counts how often the value could be an exposure (finite, above zero --
// the same test the shader applies to its live texel, falling back to the host value when it fails). Live once 60 reads
// are in and 95% held: a game whose image is not readable that way never gets past the host path.
struct LiveGameGate
{
    static constexpr unsigned long long kMinReads = 60;
    static constexpr unsigned long long kMinValidPercent = 95;
    // This many unbelievable reads in a row start the proof over: the share alone, counted over the whole session,
    // would take a long good run hundreds of bad reads to fall below 95% (the image moved to another layout, say).
    static constexpr unsigned long long kMaxBadRun = 8;

    unsigned long long reads = 0;
    unsigned long long valid = 0;
    unsigned long long badRun = 0;

    void Count(bool believed)
    {
        badRun = believed ? 0u : badRun + 1u;
        if (badRun >= kMaxBadRun)
        {
            Reset();
            return;
        }
        ++reads;
        valid += believed ? 1u : 0u;
    }

    bool Trusted() const { return reads >= kMinReads && valid * 100 >= reads * kMinValidPercent; }

    void Reset() { reads = valid = badRun = 0; }
};

// finishedPicture: the choice came from DlssNrColourEncoding::ResolveFinished (the screen), where Linear HDR is
// scRGB, display light with 1.0 = 80 nits, rather than the scene light a DLSS evaluate hands over.
inline State For(const DlssNrColourEncoding::Choice& choice, bool finishedPicture)
{
    using DlssNrColourEncoding::Encoding;

    if (choice.encoding == Encoding::Pq)
        return State::DisplayPinned;
    if (choice.encoding == Encoding::LinearHdr)
        return finishedPicture ? State::DisplayPinned : State::LinearHdr;
    return State::ToneMapped;
}
} // namespace DlssNrExposureState
