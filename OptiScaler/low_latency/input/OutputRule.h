#pragma once

// InputCommon's outputs on OptiScaler's terms: which low latency output runs, the sleep mode each one gets, and who
// keeps the frame cap while the game's own Reflex is the input. Before InputCommon ran them these decisions were spread
// over ReflexHooks' router (hooks/Reflex_Hooks.cpp) and fakenvapi's own context (nvapi/fakenvapi/low_latency_d3d.cpp);
// the policy is the same, native/XefgReflexRule.h and native/NativeLowLatencyRule.h are its rules. Pure, host tested
// (tests/ll_output_rule_smoke.cpp).

#include "../../native/XefgReflexRule.h"

#include <cstdint>

namespace lloutput
{
using native::lowlatency::XefgLatency;
using native::xefgreflex::SleepMode;

// The low latency methods InputCommon can run (LowLatencyMode's, without the Vulkan ones and the FPS limiter)
enum class Tech : uint8_t
{
    None,
    Reflex,      // the driver's (NVIDIA, real nvapi)
    AntiLag2,    // AMD
    XeLL,        // Intel's, also XeFG's on every GPU
    LatencyFlex, // works anywhere
};

struct OutputInputs
{
    bool realNvapi = false;               // NVIDIA's nvapi, not fakenvapi as the game's nvapi
    bool xefgOutput = false;              // XeFG is the frame generation output
    XefgLatency side = XefgLatency::XeLL; // with XeFG, what lowers latency (native::lowlatency::XefgSide, in force)
    bool forceLatencyFlex = false;        // fakenvapi's Force LatencyFlex
    bool multiFrame = false; // DLSS FG with more than one generated frame, or Dynamic: no Anti-Lag 2 or XeLL
};

// The output to start, the ones to try when it can't start, in order, and whether XeFG gets an XeLL context of its own
// next to it. Without XeFG fakenvapi tried Anti-Lag 2, then XeLL, then LatencyFlex (init fails on the wrong GPU).
struct OutputPlan
{
    Tech tries[3] = { Tech::None, Tech::None, Tech::None };
    bool xefgOwnXell = false; // XeFG paces with its own XeLL (low latency off), the output sleeps

    constexpr Tech first() const { return tries[0]; }
};

constexpr OutputPlan PlanOutput(const OutputInputs& in)
{
    if (in.xefgOutput)
    {
        switch (in.side)
        {
        case XefgLatency::Reflex:
            return { { Tech::Reflex, Tech::None, Tech::None }, true };
        case XefgLatency::AntiLag2:
            return { { Tech::AntiLag2, Tech::None, Tech::None }, true };
        default:
            // XeFG paces with the XeLL output's own context
            return { { Tech::XeLL, Tech::None, Tech::None }, false };
        }
    }

    if (in.realNvapi)
        return { { Tech::Reflex, Tech::None, Tech::None }, false };

    // fakenvapi as the game's nvapi: Anti-Lag 2 and XeLL fall apart with more than one generated frame
    if (in.forceLatencyFlex || in.multiFrame)
        return { { Tech::LatencyFlex, Tech::None, Tech::None }, false };

    return { { Tech::AntiLag2, Tech::XeLL, Tech::LatencyFlex }, false };
}

// Which tech a sleep mode is for
enum class Role : uint8_t
{
    Output,   // the output: it sleeps and, while it keeps it, holds the cap
    XefgXell, // XeFG's own XeLL next to a Reflex or Anti-Lag 2 output: low latency off, it only keeps XeFG running
};

struct ShapeInputs
{
    bool xefgOutput = false;
    XefgLatency side = XefgLatency::XeLL;
    uint32_t capIntervalUs = 0; // OptiScaler's frame cap for the output, in place of the input's interval; 0: none
    bool forceDisabled = false; // fakenvapi's Force Disable
    bool blocked = false;       // the Framerate page's Input is Off (native::lowlatency::GameLowLatencyBlocked)
};

// The sleep mode a tech gets from the input's own: XeLL with XeFG has low latency on whatever the game says (only Force
// Disable turns it off), the split's other side the game's own with the cap, XeFG's own XeLL everything off; Input Off
// sends low latency off, no Boost and no interval everywhere (OptiScaler's limiter keeps the cap)
constexpr SleepMode SleepModeFor(const SleepMode& input, Role role, const ShapeInputs& in)
{
    using namespace native::xefgreflex;

    const bool split = in.xefgOutput && in.side != XefgLatency::XeLL;

    if (role == Role::XefgXell)
        return XellSleepModeInSplit(SleepModeForOutput(input, in.capIntervalUs, true, in.forceDisabled, in.blocked));

    if (split && !in.blocked)
        return DriverSleepModeInSplit(input, in.capIntervalUs);

    return SleepModeForOutput(input, in.capIntervalUs, in.xefgOutput && !split, in.forceDisabled, in.blocked);
}

// The game's own Reflex as the input: who keeps OptiScaler's frame cap (the output, through its sleep mode, or
// OptiScaler's limiter) and at what rate. ReflexHooks::update's rule for D3D games.
struct GameCapInputs
{
    float capFps = 0.0f;                  // FramerateLimit (shown frames); 0: none
    bool externalFrameGeneration = false; // another frame generation owns Reflex
    bool markersRecently = false;         // the game sent a marker in the last 20 presents
    bool gameSetSleepMode = false;        // it set a sleep mode on a device that is still there
    bool gameLowLatencyOn = false;        // ... with low latency on
    bool gameSendingMarkers = false;      // its markers keep up with the frames
    bool realNvapi = false;
    bool xefgOutput = false;
    XefgLatency side = XefgLatency::XeLL;
    bool splitLimiterKeepsCap = false; // native::lowlatency::SplitLimiterKeepsCap
    bool xellHoldsCap = false;         // native::xefgreflex::XellHoldsCap (Force Disable, idle XeLL)
    bool fsrFgOutput = false;          // FSR FG is the frame generation output
    bool fgActive = false;             // frame generation shows a generated frame now (FSR FG halving)
    bool blocked = false;              // Input Off
    uint32_t xefgShownPerRendered = 1; // native::lowlatency::XefgShownPerRendered
    uint8_t dlssgFramesToGenerate = 0; // the game's DLSS FG, seen from its async markers
    bool outputIsXell = false;         // the output runs XeLL (fakenvapi's choice)
};

struct GameCap
{
    bool outputHoldsCap = false;
    float fps = 0.0f;         // the rate the output gets when it keeps the cap
    bool showWarning = false; // FSR FG with real Reflex markers and Reflex on: OptiScaler warns about input latency
};

constexpr GameCap CapForGame(const GameCapInputs& in)
{
    GameCap out;

    if (in.externalFrameGeneration || !in.markersRecently)
        return out;

    const bool split = in.xefgOutput && in.side != XefgLatency::XeLL;

    if (in.xefgOutput && (in.gameSetSleepMode || in.gameSendingMarkers))
    {
        if (split)
        {
            // A game that marks frames and never sleeps: only Anti-Lag 2 (fakenvapi) acts on the markers alone
            out.outputHoldsCap =
                !in.splitLimiterKeepsCap && (!in.realNvapi || (in.gameSetSleepMode && in.gameLowLatencyOn));
        }
        else
        {
            out.outputHoldsCap = in.xellHoldsCap;
        }
    }
    else if (in.gameSetSleepMode)
    {
        // Real Reflex markers with FSR FG and Reflex off: huge input latency, the limiter keeps it
        out.outputHoldsCap = !in.realNvapi || !in.fgActive || in.gameLowLatencyOn;
        out.showWarning = in.fsrFgOutput && in.realNvapi && in.fgActive && in.gameLowLatencyOn;
    }

    if (in.blocked)
    {
        out.outputHoldsCap = false;
        out.showWarning = false;
    }

    if (!out.outputHoldsCap)
        return out;

    float fps = in.capFps;

    if (in.fgActive && in.fsrFgOutput)
        fps /= 2;
    else if (split)
        fps = native::lowlatency::CapPerRenderedFrame(fps, in.xefgShownPerRendered);
    else if (in.dlssgFramesToGenerate > 0 && !in.realNvapi && !in.outputIsXell)
        fps /= (float) (in.dlssgFramesToGenerate + 1);

    out.fps = fps;
    return out;
}

// The interval a sleep mode carries for `fps`, 0: no cap
constexpr uint32_t IntervalUs(float fps) { return fps > 0.0f ? static_cast<uint32_t>(1'000'000.0f / fps + 0.5f) : 0; }
} // namespace lloutput
