#pragma once

// Constants of precompile/dlssnr_edit_cache.hlsl, the edit cache (dlssnr/DlssNrEditCache.h): the model runs one frame
// in N and the frames between show its last answer, carried along the game's vectors. The design follows the edit cache
// of Skynizz/optiscaler-dlss5 (its design notes, edit-cache.md); this code is written here from those notes.
//
// What is carried is the model's edit as a ratio of light, log2((answer + eps) / (frame + eps)) per channel, taken on
// the frame the model ran on (the anchor). Each later frame reads it from the anchor through the vectors chained back
// frame by frame -- a ring of the last frames' vectors as uv displacements -- so it is read once however far it was
// carried, never resampled from frame to frame. A pixel whose anchor fingerprint no longer fits this frame's 3x3
// range (or whose surroundings changed) is dropped until the next refresh and gets the low band of its own surface.
//
// No dependencies, so tests/nr_edit_cache_shader_smoke.cpp can fill it too. Same 256-byte slot as DlssNrConstants, so
// it goes through DlssNr_Dx12's constant-buffer ring and its shared root signature (t0-t5, u0-u1, one sampler).

#include <cstdint>

enum DlssNrEditCacheMode : uint32_t
{
    // [ring] t0 motion guide, t1 depth guide -> u0 ring slice RingSlice: this frame's vectors as uv displacement to
    //        the previous frame, from the nearest surface of the 3x3 around each texel where depth is there. The
    //        ring is a grid of its own (RingWidth x RingHeight a slice), read by uv, so a render-size change does not
    //        disturb it.
    DlssNrEditCache_Ring = 0,
    // [quarter] t0 frame -> u0 mean log luma of each 4x4 block (the surroundings the context check compares)
    DlssNrEditCache_Context = 1,
    // [output] t0 frame, t1 the model's result, t4 this frame's context -> u0 anchor edit (rgb, stops),
    //          u1 anchor fingerprint (log luma, two log chroma ratios, the surroundings' log luma)
    DlssNrEditCache_Capture = 2,
    // [output] t0 frame, t1 ring, t2 anchor edit, t3 anchor fingerprint, t4 this frame's context, t5 last frame's
    //          validity -> u0 carried edit (rgb) and validity (a), u1 this frame's validity (r)
    DlssNrEditCache_Carry = 3,
    // [cells] t0 frame, t1 carried edit -> u0 cell atlas (mean edit rgb, coverage), u1 cell atlas (mean log luma):
    //          one level (CellLevel) of the surface fill's grid. Level 0 from its own pixels; the coarser levels from
    //          level 0's cells (t2 edit, t3 luma: a copy of the atlases after level 0), weighted by how many of their
    //          pixels were validated, which gives the same sums
    DlssNrEditCache_Cells = 4,
    // [output] t0 frame, t1 carried edit (rgb, validity a), t2 cell atlas edit, t3 cell atlas luma, t4 fade source,
    //          t5 ring -> u0 the output, frame x 2^edit, u1 the edit shown (rgb) and this frame's log luma (a), for
    //          the next refresh's fade, which blends only where that luma still fits the frame's 3x3 range
    DlssNrEditCache_Apply = 5,
    // [motion] t1 ring -> u0 raw vectors from this frame back to the anchor, in the game's units at the motion
    //          texture's allocation size (the model's own history lands where the scene went on a refresh)
    DlssNrEditCache_Compose = 6,
};

// Slices in the ring of vectors: enough for the longest interval (8) to trace back to its anchor, and for a crossfade
// to trace back to the frame before its refresh. Laid out 4 across and 2 down, so a ring of 4K vectors stays inside
// D3D12's 16384-texel limit (8 down did not: 8 x 2160 rows).
constexpr uint32_t kDlssNrEditCacheRingSlices = 8;
constexpr uint32_t kDlssNrEditCacheRingColumns = 4;
constexpr uint32_t kDlssNrEditCacheRingRows = kDlssNrEditCacheRingSlices / kDlssNrEditCacheRingColumns;
// The surface fill's grid: cells of 8, 32 and 128 output pixels.
constexpr uint32_t kDlssNrEditCacheCellLevels = 3;
constexpr uint32_t kDlssNrEditCacheCellSize[kDlssNrEditCacheCellLevels] = { 8, 32, 128 };

struct alignas(256) DlssNrEditCacheConstants
{
    uint32_t Mode;
    uint32_t Width; // output size
    uint32_t Height;
    uint32_t MotionWidth; // the motion subrect, and where it sits in its texture
    uint32_t MotionHeight;
    uint32_t MotionBaseX;
    uint32_t MotionBaseY;
    uint32_t DepthWidth; // 0: no depth this frame
    uint32_t DepthHeight;
    uint32_t DepthBaseX;
    uint32_t DepthBaseY;
    uint32_t DepthInverted;
    float MvScaleX; // the game's scale: raw vectors -> pixels of the motion subrect
    float MvScaleY;
    uint32_t Passthrough;   // 1: the frame is display-encoded (sRGB), decoded to light for the ratio and encoded back
    float Epsilon;          // the ratio's floor, in the frame's light: white point / 512
    uint32_t RingSlice;     // Ring: the slice written; Carry / Apply / Compose: the slice of this frame
    uint32_t TraceSteps;    // Carry / Compose: frames back to the anchor (1 = the anchor is last frame)
    uint32_t FadeSteps;     // Apply: frames back to the fade source (0 = no crossfade on this frame)
    float FadeWeight;       // Apply: how much of the carried (new) edit is shown, the rest the fade source (1 = none)
    float Tolerance;        // the fingerprint's tolerance outside this frame's 3x3 range, stops
    float ContextTolerance; // the surroundings' tolerance, stops
    uint32_t FirstCarried;  // Carry: last frame was the anchor, so there is no earlier validity to read
    uint32_t CellLevel;     // Cells / Apply: which level (Cells), 0
    // Where each level sits in the cell atlases (x; y is 0), and its cells across and down. Scalars, not arrays: an
    // HLSL cbuffer gives every array element a 16-byte register of its own.
    uint32_t CellAtlasX0, CellAtlasX1, CellAtlasX2;
    uint32_t CellCountX0, CellCountX1, CellCountX2;
    uint32_t CellCountY0, CellCountY1, CellCountY2;
    float FillSigma;       // the surface fill's brightness tolerance (Gaussian sigma), stops
    uint32_t DebugView;    // Apply: 1 paints dropped pixels red, filled from the surface; 2 shows the edit's age
    uint32_t ComposeWidth; // Compose: the motion texture's allocation size it writes
    uint32_t ComposeHeight;
    float Age;          // Apply's debug view 2: frames since the anchor / interval
    uint32_t RingWidth; // one slice of the ring (the motion texture's allocation size, grown only)
    uint32_t RingHeight;
};
static_assert(sizeof(DlssNrEditCacheConstants) == 256);

// Defaults (measured offline on an NBA 2K27 dump, 2026-10-13: DLSS-TOOLS editcache/cache_sim.py). The crossfade of 5
// brought the refresh frame's step of regional light down to the model's own every-frame step at an interval of 8.
constexpr float kDlssNrEditCacheTolerance = 0.25f;
constexpr float kDlssNrEditCacheContextTolerance = 0.15f;
constexpr float kDlssNrEditCacheFillSigma = 0.35f;
