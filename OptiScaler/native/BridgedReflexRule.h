#pragma once

// A Vulkan game on the present bridge (native/VkPresentBridge.h) runs XeFG and its XeLL on the bridge's private D3D12
// device, while the game's own Reflex calls (NvAPI_Vulkan_*, VK_NV_low_latency2) go to the driver. The same calls on
// D3D12 are sent to XeLL by hooks/Reflex_Hooks.cpp when XeFG is the output; these are the rules for doing it for the
// bridged Vulkan game. No Windows, no Vulkan, no GPU: a host test can include it.

#include <cstdint>

namespace native::bridgedreflex
{
// The game's Vulkan Reflex calls feed XeFG's XeLL on the bridge's D3D12 device. XeFG has to be the output and the bridge's
// device has to be up. Never for a game on dxvk without the Vulkan bridge: its Reflex is not Vulkan Reflex that the bridge
// would own (the bridge is not up for it, and the D3D11 side has its own routing).
inline bool ShouldForward(bool xefgOutput, bool bridgeDevicePresent, bool dxvkWithoutVulkanBridge)
{
    return xefgOutput && bridgeDevicePresent && !dxvkWithoutVulkanBridge;
}

// NV_LATENCY_MARKER_TYPE (and NV_VULKAN_LATENCY_MARKER_TYPE, which has the same values) as XeLL's fakenvapi side takes it.
inline constexpr uint32_t kNvApiPcLatencyPing = 8;
inline constexpr uint32_t kNvApiLastMarker = 12; // OUT_OF_BAND_PRESENT_END

// VkLatencyMarkerNV 0-7 (simulation start .. trigger flash) are NVAPI's 0-7. NVAPI has PC_LATENCY_PING at 8, which Vulkan
// does not have, so VK_LATENCY_MARKER_OUT_OF_BAND_* 8-11 are NVAPI's 9-12. False for a value that is neither.
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
inline bool XellTakesMarker(uint32_t nvapiMarker)
{
    return nvapiMarker <= 5;
}

struct SleepMode
{
    bool lowLatency = false;
    bool boost = false;
    uint32_t minimumIntervalUs = 0; // 0: no limit
    bool useMarkersToOptimize = false;
};

struct SleepModeSplit
{
    SleepMode xell;   // what XeLL gets
    SleepMode driver; // what the driver gets
};

// The game's sleep mode goes to XeLL as the game set it, with OptiScaler's frame cap in place of its interval when one is
// set (as hkNvAPI_D3D_SetSleepMode does). The driver gets the mode with everything off, including the interval: only one
// of the two may wait, and the driver's Sleep must still return and signal the game's semaphore.
inline SleepModeSplit SplitSleepMode(const SleepMode& game, uint32_t capIntervalUs)
{
    SleepModeSplit split;
    split.xell = game;

    if (capIntervalUs != 0)
        split.xell.minimumIntervalUs = capIntervalUs;

    return split;
}

} // namespace native::bridgedreflex
