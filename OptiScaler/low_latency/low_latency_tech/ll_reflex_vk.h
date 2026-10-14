#pragma once

#include "low_latency_tech.h"

#include <low_latency/input/OutputRule.h>
#include <native/NativeLowLatencyVkSync.h>

#include <vulkan/vulkan.h>

// VK_NV_low_latency2 has no PC latency ping, the out of band markers come one earlier (OutputRule.h, host tested)
inline std::optional<VkLatencyMarkerNV> ToVkLatencyMarker(MarkerType type)
{
    if (uint32_t marker = 0; lloutput::MarkerToVulkanNV((uint32_t) type, &marker))
        return (VkLatencyMarkerNV) marker;

    return std::nullopt;
}

inline std::optional<MarkerType> FromVkLatencyMarker(VkLatencyMarkerNV marker)
{
    if (uint32_t type = 0; lloutput::MarkerFromVulkanNV((uint32_t) marker, &type))
        return (MarkerType) type;

    return std::nullopt;
}

// NVIDIA Reflex for Vulkan through VK_NV_low_latency2 (nvapi's Vulkan Reflex is deprecated). Works on the swapchain
// created with low latency mode: VulkanHooks chains it into every swapchain of a device made with the extension
// (VulkanSpoofing adds it on NVIDIA). Where that can't be, InputCommon runs ll_reflex_nvapi_vk.h (OutputRule.h
// ChooseReflexVk).
class ReflexVk : public LowLatencyTech
{
    VkDevice device = VK_NULL_HANDLE;
    VkSemaphore semaphore = VK_NULL_HANDLE;
    uint64_t semaphore_value = 0;

    SleepMode sleep_mode {};
    bool sleep_mode_known = false;
    bool applied_enabled = false;
    VkSwapchainKHR applied_swapchain = VK_NULL_HANDLE; // Sleep mode is per swapchain

    // Waits in a row that timed out, whether one ever reached its value, and given up on a driver that never signals
    // (the same policy as native/NativeLowLatencyVkSync.h)
    uint32_t timeouts = 0;
    bool ever_signalled = false;
    bool gave_up = false;

    PFN_vkWaitSemaphores o_WaitSemaphores = nullptr;
    PFN_vkDestroySemaphore o_DestroySemaphore = nullptr;

    // On the device's swapchain with latency mode (VulkanHooks::LatencySwapchainUse holds it)
    void apply_sleep_mode(VkSwapchainKHR swapchain);

  public:
    ReflexVk() : LowLatencyTech() {}

    // From LowLatencyTech
    bool init(IUnknown* pDevice) override;
    void deinit() override;

    LowLatencyMode get_mode() override { return LowLatencyMode::Reflex; };
    void* get_tech_context() override { return device; };
    void set_fg_type(bool interpolated, uint64_t frame_id) override {}; // Out of band markers carry this for Reflex
    void set_low_latency_override(ForceReflex low_latency_override) override
    {
        this->low_latency_override = low_latency_override;
    };
    void set_effective_fg_state(bool effective_fg_state) override { this->effective_fg_state = effective_fg_state; };

    bool is_enabled() override;

    void get_sleep_status(SleepParams* sleep_params) override;
    void set_sleep_mode(SleepMode* sleep_mode) override;
    void sleep(std::optional<uint32_t> frame_id) override;
    void set_marker(IUnknown* pDevice, const MarkerParams& marker_params) override;
    void set_async_marker(IUnknown* pCommandQueue, const MarkerParams& marker_params) override;

    // Its sleep never signalled: the device's VK_NV_low_latency2 doesn't work (OutputRule.h ChooseReflexVk)
    bool gave_up_sleeping() const { return gave_up; }

    // For passthrough
    VkResult get_latency(VkGetLatencyMarkerInfoNV* latency_info);
    // The driver's timings in NVAPI's report layout (the menu, a game's NvAPI GetLatency)
    NvAPI_Status get_latency(NV_LATENCY_RESULT_PARAMS* latency_params);
    void notify_out_of_band(VkQueue queue, VkOutOfBandQueueTypeNV queue_type) const;
};
