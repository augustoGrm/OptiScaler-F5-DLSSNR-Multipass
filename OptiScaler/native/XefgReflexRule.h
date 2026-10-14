#pragma once

// A game's own Reflex while XeFG is the frame generation output: hooks/Reflex_Hooks.cpp sends its calls to XeFG's XeLL
// (fakenvapi's), on D3D through the D3D hooks and for a Vulkan game on the present bridge (native/VkPresentBridge.h) on
// the bridge's private D3D12 device, where the game's NvAPI_Vulkan_* and VK_NV_low_latency2 calls would otherwise only
// reach the driver. These are the rules for both. No Windows, no Vulkan, no GPU: a host test can include it.

#include "NativeLowLatencyRule.h"

#include <cstdint>

namespace native::xefgreflex
{
// The game's Vulkan Reflex calls feed XeFG's XeLL on the bridge's D3D12 device. XeFG has to be the output and the
// bridge's device has to be up. Never for a game on dxvk without the Vulkan bridge: its Reflex is not Vulkan Reflex
// that the bridge would own (the bridge is not up for it, and the D3D11 side has its own routing).
inline bool ShouldForward(bool xefgOutput, bool bridgeDevicePresent, bool dxvkWithoutVulkanBridge)
{
    return xefgOutput && bridgeDevicePresent && !dxvkWithoutVulkanBridge;
}

// NV_LATENCY_MARKER_TYPE (NV_VULKAN_LATENCY_MARKER_TYPE has the same values) as XeLL's fakenvapi side takes it.
inline constexpr uint32_t kNvApiPcLatencyPing = 8;
inline constexpr uint32_t kNvApiLastMarker = 12; // OUT_OF_BAND_PRESENT_END

// VkLatencyMarkerNV 0-7 (simulation start .. trigger flash) are NVAPI's 0-7. NVAPI has PC_LATENCY_PING at 8, which
// Vulkan does not have, so VK_LATENCY_MARKER_OUT_OF_BAND_* 8-11 are NVAPI's 9-12. False for a value that is neither.
inline bool NvApiMarkerFromVulkanNV(uint32_t vkMarker, uint32_t* nvapiMarker)
{
    if (vkMarker < kNvApiPcLatencyPing)
        *nvapiMarker = vkMarker;
    else if (vkMarker <= 11)
        *nvapiMarker = vkMarker + 1;
    else
        return false;

    return true;
}

// The markers XeLL takes (fakenvapi's XeLL: the six frame markers). The driver still gets every one the game sent.
inline bool XellTakesMarker(uint32_t nvapiMarker) { return nvapiMarker <= 5; }

struct SleepMode
{
    bool lowLatency = false;
    bool boost = false;
    uint32_t minimumIntervalUs = 0; // 0: no limit
    bool useMarkersToOptimize = false;
};

// The sleep mode the game's Reflex output gets. OptiScaler's frame cap replaces the game's interval when one is set.
// While XeFG is the output (its XeLL, D3D or the bridge), low latency is on whatever the game's own Reflex setting
// says: XeFG always uses XeLL, fakenvapi's XeLL skips every sleep and marker while the mode it was given is off, and
// its frame cap waits in those sleeps (Reflex off in RDR2's menu left both doing nothing). Only the user turns it off,
// with fakenvapi's Force Disable. Not written for XeLL alone: whichever low latency XeFG is paired with gets the same.
// `blockLowLatency` (the Framerate page's Input: Off, any output): low latency off, no Boost, no interval whatever the
// game or the cap says; OptiScaler's limiter keeps the cap (XellHoldsCap and ReflexHooks::update do not hand it over).
inline SleepMode SleepModeForOutput(const SleepMode& game, uint32_t capIntervalUs, bool xefgOutput, bool forceDisabled,
                                    bool blockLowLatency = false)
{
    SleepMode out = game;

    if (blockLowLatency)
    {
        out.lowLatency = false;
        out.boost = false;
        out.minimumIntervalUs = 0;
        return out;
    }

    if (capIntervalUs != 0)
        out.minimumIntervalUs = capIntervalUs;

    if (xefgOutput)
        out.lowLatency = !forceDisabled;

    return out;
}

// A bridged game's driver while XeLL takes its sleep mode: everything off, the interval too. Only one of the two may
// wait, and the driver's Sleep must still return and signal the game's semaphore.
inline constexpr SleepMode kDriverSleepMode {};

// ---- XeFG with Reflex or Anti-Lag 2 as the low latency output ("split") ----
//
// XeFG needs an XeLL context and its frame IDs follow the markers sent (framegen/xefg/XeFGFrameId.h), so XeLL stays in
// place and every call still reaches it, but with its low latency off: fakenvapi's XeLL skips every sleep and marker
// while its mode is off (as with Force Disable), and XeFG keeps running. The latency work is the other side's: the
// driver's Reflex (real nvapi) or Anti-Lag 2 / Anti-Lag Vk (fakenvapi's, a second context next to XeLL's) gets the
// game's real mode, markers and Sleep, the way FSR FG's Reflex does.

using lowlatency::XefgLatency;

// The side that lowers latency right now. XeLL is today's behaviour and the answer whenever the setup lacks the choice,
// XeFG is not the output, the Framerate page's Input is Off (nothing to lower) or fakenvapi's Force Reflex is set
// (Force Disable wants everything off; Force Enable would make XeLL sleep next to the other side).
inline XefgLatency EffectiveXefgLatency(const lowlatency::OutputInputs& in, bool blockLowLatency, bool forceReflexSet)
{
    if (!in.xefgOutput || blockLowLatency || forceReflexSet || !lowlatency::XefgLatencyAvailable(in.xefgLatency, in))
        return XefgLatency::XeLL;

    return in.xefgLatency;
}

enum class Call
{
    SetSleepMode,
    Sleep,
    Marker,
    GetLatency
};

struct Route
{
    bool xell = false;   // XeFG's XeLL (fakenvapi's, on the device XeFG made it on)
    bool driver = false; // the other side: the driver's Reflex, or Anti-Lag 2 / Anti-Lag Vk (not for XeLL)
};

// Where one Reflex call goes. A bridged Vulkan game's semaphore is signalled by the other side's Sleep (the driver's,
// or fakenvapi's NvAPI_Vulkan_Sleep): with XeLL as the output it is signalled from the host instead
// (VK_NV_low_latency2) or by the driver's Sleep with its mode off (NvAPI_Vulkan).
inline Route RouteFor(XefgLatency side, Call call)
{
    if (side == XefgLatency::XeLL)
        return { true, false };

    switch (call)
    {
    case Call::Sleep:
    case Call::GetLatency:
        return { false, true }; // XeLL never sleeps here, and the driver has the real GPU times
    default:
        return { true, true };
    }
}

// The game's sleep mode as XeLL gets it: low latency off, no Boost, no interval (it neither sleeps nor holds the cap)
inline SleepMode XellSleepModeInSplit(const SleepMode& game)
{
    SleepMode out = game;
    out.lowLatency = false;
    out.boost = false;
    out.minimumIntervalUs = 0;
    return out;
}

// ...and as the other side gets it: the game's own, the cap in place of its interval when one is set
inline SleepMode DriverSleepModeInSplit(const SleepMode& game, uint32_t capIntervalUs)
{
    SleepMode out = game;

    if (capIntervalUs != 0)
        out.minimumIntervalUs = capIntervalUs;

    return out;
}

// Whether XeFG's XeLL keeps the frame cap the game's Reflex hands it: while it is not idle (NativeLowLatencyRule.h
// XellIdle: on NVIDIA and AMD it does nothing while XeFG generates no frames) and not turned off with fakenvapi's
// Force Disable. OptiScaler's limiter keeps it otherwise.
inline bool XellHoldsCap(bool forceDisabled, bool intelGpu, bool xefgGenerating)
{
    lowlatency::CapInputs in;
    in.method = lowlatency::Method::XellWithXefg;
    in.intelGpu = intelGpu;
    in.xefgGenerating = xefgGenerating;
    return !forceDisabled && !lowlatency::XellIdle(in);
}

} // namespace native::xefgreflex
