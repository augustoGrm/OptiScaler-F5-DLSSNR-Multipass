#pragma once

// VK_NV_low_latency2's latency mode on a swapchain: InputCommon's Reflex output on that extension (ll_reflex_vk.h) only
// works on a swapchain made with it, so VulkanHooks chains it into the swapchains of a device made with the extension.
// No hooks, no device: a host test drives it (tests/ll_output_rule_smoke.cpp).

#include <vulkan/vulkan_core.h>

namespace lloutput
{
// Whether the swapchain `info` makes will have the latency mode. On a device with VK_NV_low_latency2 the game's own
// choice stays (its VkSwapchainLatencyCreateInfoNV, true or false); without one ours goes at the head of the chain.
// `latency` must outlive the create call.
inline bool ChainLatencyMode(bool lowLatency2Device, VkSwapchainCreateInfoKHR& info,
                             VkSwapchainLatencyCreateInfoNV& latency)
{
    if (!lowLatency2Device)
        return false;

    for (auto* node = static_cast<const VkBaseInStructure*>(info.pNext); node != nullptr; node = node->pNext)
    {
        if (node->sType == VK_STRUCTURE_TYPE_SWAPCHAIN_LATENCY_CREATE_INFO_NV)
            return reinterpret_cast<const VkSwapchainLatencyCreateInfoNV*>(node)->latencyModeEnable != VK_FALSE;
    }

    latency = {};
    latency.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_LATENCY_CREATE_INFO_NV;
    latency.pNext = info.pNext;
    latency.latencyModeEnable = VK_TRUE;
    info.pNext = &latency;
    return true;
}
} // namespace lloutput
