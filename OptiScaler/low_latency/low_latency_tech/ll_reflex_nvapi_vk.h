#pragma once

#include "low_latency_tech.h"

#include <native/NativeLowLatencyVkSync.h>

#include <vulkan/vulkan.h>

// NVIDIA Reflex on a Vulkan device through the driver's NvAPI_Vulkan_* (the interface before VK_NV_low_latency2), past
// the hooks (ReflexHooks::nvapiVk*). The device is made a low latency device once, which hands back a timeline
// semaphore, and each frame's Sleep gives a value the driver signals on it; the sleep waits for it here
// (native/NativeLowLatencyVkSync.h). It needs the device alone, no swapchain: InputCommon's Reflex on Vulkan where
// VK_NV_low_latency2 can't run (OutputRule.h ChooseReflexVk), e.g. on dxvk's device or one made before our hooks.
class ReflexNvApiVk : public LowLatencyTech
{
    VkDevice device = VK_NULL_HANDLE;
    SleepMode sleep_mode {};
    bool sleep_mode_known = false;
    bool applied_enabled = false;

    void apply_sleep_mode();

  public:
    ReflexNvApiVk() : LowLatencyTech() {}

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

    // The driver's report
    NvAPI_Status get_latency(NV_VULKAN_LATENCY_RESULT_PARAMS* latency_params) const;

    // The device is gone: its low latency state (the driver's semaphore and the last value) goes with it. A device
    // keeps it across outputs otherwise: the driver makes a device a low latency one once, and the values only go up.
    static void forget_device(VkDevice device);
};
