#pragma once

// The edit cache's rules, free of the GPU and the config so tests/nr_edit_cache_smoke.cpp can pin them: when it runs,
// which frames the model runs on (refresh) and which show its carried edit (cached), what the passes are told (ring
// slice, frames back to the anchor, the crossfade), and why it stands aside when it does. The GPU side is
// shaders/dlssnr/DlssNr_EditCache.inl and precompile/dlssnr_edit_cache.hlsl. Design after the edit cache of
// Skynizz/optiscaler-dlss5 (edit-cache.md), written here from those notes.

#include <cstdint>

namespace DlssNrEditCache
{
constexpr uint32_t kRingSlices = 8;
constexpr uint32_t kMinInterval = 2;
constexpr uint32_t kMaxInterval = 8;  // the ring holds the frames back to the anchor
constexpr uint32_t kMaxCrossfade = 8; // and back to the frame before a refresh
// A frame clock that jumps by more than this since the last frame the cache saw: frames went by without their vectors
// in the ring (NR was off, a menu), so the next frame is a refresh. Frame generation steps the clock by 2 per rendered
// frame (one presented in between), 3 or 4 at 3x/4x.
constexpr unsigned long long kMaxClockStep = 4;

struct Inputs
{
    bool enabled = false;       // [DlssNr] EditCache
    uint32_t interval = 4;      // [DlssNr] EditCacheInterval: the model every N frames
    uint32_t crossfade = 5;     // [DlssNr] EditCacheCrossfade: frames a refresh takes to reach the new answer (1 = at once)
    bool detailReuse = false;   // Reuse detail (every other frame) is on: the two do not run together
    bool afterUpscale = true;   // NR runs after the upscaler on the game's own frame
    bool targetWritable = true; // the output takes a UAV (the cache writes it in place)
    bool workAbove100 = false;  // Model resolution above 100%
    bool edgeCompression = false;
    bool convertedEncoding = false; // a forced gamma 2.2 / PQ encoding converted at the pass's edges (not supported)
    bool blocked = false;           // Tune, frame hold, a capture: the real model must run
    bool debugOrCompare = false;    // a debug view or Compare is on: they show the model's own answer
    bool reset = false;             // the game reset its history (a cut) or the model starts over
    unsigned long long revision = 0; // changes when anything that changes the model's answer changes
    uint32_t width = 0, height = 0;  // output size
    uint32_t motionWidth = 0, motionHeight = 0;
    unsigned long long clock = 0;    // the frame clock (present epoch)
};

// What to do on this frame.
struct Step
{
    bool runs = false;     // the cache is on for this frame (its ring is written)
    bool refresh = false;  // the model runs; its answer becomes the anchor
    bool cached = false;   // the model is skipped; the carried edit is shown
    uint32_t ringSlice = 0;
    uint32_t traceSteps = 0;   // cached: frames back to the anchor; refresh: frames back to the last model run (compose)
    bool firstCarried = false; // cached: last frame was the anchor
    bool composeForModel = false; // refresh after cached frames: hand the model vectors back to its last run
    uint32_t fadeSteps = 0;    // frames back to the fade source (0 = no crossfade on this frame)
    float fadeWeight = 1.0f;   // how much of the new edit is shown
    float age = 0.0f;          // frames since the anchor over the interval (debug view)
    const char* standsAside = nullptr; // why it does not run (null while it runs or is off)
};

inline const char* StandAsideReason(const Inputs& in)
{
    if (in.detailReuse)
        return "Reuse detail (every other frame) is on";
    if (!in.afterUpscale)
        return "NR runs before the upscaler or on the finished picture (after the upscaler only, for now)";
    if (!in.targetWritable)
        return "the output cannot be written in place";
    if (in.workAbove100)
        return "Model resolution is above 100%";
    if (in.edgeCompression)
        return "Compress screen edges is on";
    if (in.convertedEncoding)
        return "a forced gamma 2.2 or PQ colour encoding";
    if (in.blocked)
        return "Tune, frame hold or a capture needs the model on every frame";
    if (in.debugOrCompare)
        return "a debug view or Compare is on";
    if (in.width == 0 || in.height == 0 || in.motionWidth == 0 || in.motionHeight == 0)
        return "no size";
    return nullptr;
}

class Cadence
{
  public:
    Step Next(const Inputs& in)
    {
        Step s;
        if (!in.enabled)
        {
            Drop();
            return s;
        }
        s.standsAside = StandAsideReason(in);
        if (s.standsAside != nullptr)
        {
            Drop();
            return s;
        }

        const uint32_t interval = Clamp(in.interval, kMinInterval, kMaxInterval);
        const uint32_t crossfade = Clamp(in.crossfade, 1, kMaxCrossfade);
        const bool gap = _seen && (in.clock < _clock || in.clock - _clock > kMaxClockStep);
        const bool changed = _seen && (in.revision != _revision || in.width != _width || in.height != _height ||
                                       in.motionWidth != _motionWidth || in.motionHeight != _motionHeight);

        _slice = _seen ? (_slice + 1) % kRingSlices : 0;
        s.runs = true;
        s.ringSlice = _slice;

        const bool mustRefresh = !_anchored || in.reset || gap || changed || _sinceAnchor + 1 >= interval;
        if (mustRefresh)
        {
            // The model's own history: vectors back to its last run, when that was more than a frame ago and the
            // frames between are still in the ring
            const bool continuous = _anchored && !in.reset && !gap && !changed;
            s.composeForModel = continuous && _sinceAnchor + 1 > 1;
            s.traceSteps = s.composeForModel ? _sinceAnchor + 1 : 0;
            // Crossfade from what was shown, unless the picture just started over
            _fadeFrom = continuous && crossfade > 1 ? 0u : kNoFade;
            _anchored = true;
            _sinceAnchor = 0;
            s.refresh = true;
        }
        else
        {
            ++_sinceAnchor;
            s.cached = true;
            s.traceSteps = _sinceAnchor;
            s.firstCarried = _sinceAnchor == 1;
            if (_fadeFrom != kNoFade)
                ++_fadeFrom;
        }

        if (_fadeFrom != kNoFade)
        {
            const uint32_t k = _fadeFrom; // frames since the refresh
            if (k + 1 < crossfade)
            {
                s.fadeSteps = k + 1;
                s.fadeWeight = float(k + 1) / float(crossfade);
            }
            else
                _fadeFrom = kNoFade;
        }

        s.age = float(_sinceAnchor) / float(interval);
        _seen = true;
        _clock = in.clock;
        _revision = in.revision;
        _width = in.width;
        _height = in.height;
        _motionWidth = in.motionWidth;
        _motionHeight = in.motionHeight;
        ++_frames;
        _modelFrames += s.refresh ? 1 : 0;
        return s;
    }

    // Start over: the next frame that runs is a refresh with nothing to fade from.
    void Drop()
    {
        _seen = false;
        _anchored = false;
        _sinceAnchor = 0;
        _fadeFrom = kNoFade;
    }

    // Share of the frames the cache ran on that ran the model (for the menu), and reset the count.
    float TakeModelShare()
    {
        const float share = _frames != 0 ? float(_modelFrames) / float(_frames) : 0.0f;
        _frames = _modelFrames = 0;
        return share;
    }

  private:
    static constexpr uint32_t kNoFade = 0xFFFFFFFFu;
    static uint32_t Clamp(uint32_t v, uint32_t lo, uint32_t hi) { return v < lo ? lo : v > hi ? hi : v; }

    bool _seen = false;
    bool _anchored = false;
    uint32_t _slice = 0;
    uint32_t _sinceAnchor = 0;
    uint32_t _fadeFrom = kNoFade;
    unsigned long long _clock = 0;
    unsigned long long _revision = 0;
    uint32_t _width = 0, _height = 0, _motionWidth = 0, _motionHeight = 0;
    unsigned long long _frames = 0, _modelFrames = 0;
};
} // namespace DlssNrEditCache
