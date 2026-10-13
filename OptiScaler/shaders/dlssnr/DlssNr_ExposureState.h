#pragma once

// Whether the NR Exposure controls (white point source, Trim, Automatic, Paper white, Tune) decide how bright the
// picture handed to the model is, on the frame the pass last ran on. Three cases:
//
//   LinearHdr      the frame is open-ended linear light; the white point the controls pick divides it before the
//                  proxy curve. The only case the controls act in.
//   ToneMapped     the game already tone mapped the frame (an SDR picture): it reaches the model as it is, with no
//                  white point at all (dlssnr.hlsl's passthrough). Paper white then only scales the debug views.
//   DisplayPinned  display-referred HDR -- a forced PQ frame on the game-input path, or Finished Picture on an scRGB or
//                  HDR10 screen. White is the game's HDR paper white (ITU-R BT.2408's 203 nits by default), not an
//                  exposure, so the controls are skipped.
//
// Decided from the colour encoding choice (DlssNr_ColourEncoding.h) alone, so the menu can say which case it is
// without asking the backend. Header-only and free of D3D and Vulkan types (tests/nr_exposure_state_smoke.cpp).

#include "DlssNr_ColourEncoding.h"

#include <cstdint>

namespace DlssNrExposureState
{
enum class State : uint32_t
{
    LinearHdr,
    ToneMapped,
    DisplayPinned,
};

// ITU-R BT.2408's reference white, the white a display-referred HDR frame is put at.
constexpr float kReferenceWhiteNits = 203.0f;

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
