#include "pch.h"
#include "input_reflex_vk.h"

#include <hooks/Reflex_Hooks.h>
#include <hooks/Vulkan_Hooks.h>
#include <low_latency/low_latency_tech/ll_reflex_vk.h>

#include <intrin.h>

#pragma intrinsic(_ReturnAddress)

static void LogResult(InputResult result, const char* call)
{
    if (result != InputResult::Ok && result != InputResult::UsingDifferentInput &&
        result != InputResult::NotEnoughReports && result != InputResult::NoReadyOutput &&
        result != InputResult::InputNotSupported)
    {
        LOG_ERROR("{} result: {}", call, magic_enum::enum_name(result));
    }
}

VkResult VKAPI_CALL InputReflexVk::SetLatencySleepMode(VkDevice device, VkSwapchainKHR swapchain,
                                                       const VkLatencySleepModeInfoNV* pSleepModeInfo)
{
    if (device == VK_NULL_HANDLE || swapchain == VK_NULL_HANDLE)
        return VK_ERROR_INITIALIZATION_FAILED;

    // The game's swapchain has the latency mode (the game made it so): the Reflex output on VK_NV_low_latency2 uses it
    VulkanHooks::NoteLatencySwapchain(device, swapchain);

    // Without the info low latency is off
    SleepMode sleepMode {};
    sleepMode.use_markers_to_optimize = true;

    if (pSleepModeInfo != nullptr)
    {
        sleepMode.low_latency_enabled = pSleepModeInfo->lowLatencyMode;
        sleepMode.low_latency_boost = pSleepModeInfo->lowLatencyBoost;
        sleepMode.minimum_interval_us = pSleepModeInfo->minimumIntervalUs;
    }

    ReflexHooks::preVkSetSleepMode((HANDLE) device, sleepMode.low_latency_enabled,
                                   ReflexHooks::isGameCall(_ReturnAddress()));

    LogResult(InputCommon::set_sleep_mode(inputContext, (IUnknown*) device, &sleepMode), "set_sleep_mode");
    return VK_SUCCESS;
}

VkResult VKAPI_CALL InputReflexVk::LatencySleep(VkDevice device, VkSwapchainKHR swapchain,
                                                const VkLatencySleepInfoNV* pSleepInfo)
{
    if (device == VK_NULL_HANDLE || swapchain == VK_NULL_HANDLE || pSleepInfo == nullptr)
        return VK_ERROR_INITIALIZATION_FAILED;

    VulkanHooks::NoteLatencySwapchain(device, swapchain);
    ReflexHooks::preVkSleep((HANDLE) device, ReflexHooks::isGameCall(_ReturnAddress()));

    LogResult(InputCommon::sleep(inputContext, (IUnknown*) device), "sleep");

    // The game waits for it whatever the output did; the Reflex output's sleep has a semaphore of its own
    if (!VulkanHooks::SignalTimelineSemaphore(device, pSleepInfo->signalSemaphore, pSleepInfo->value) &&
        VulkanHooks::o_vkLatencySleepNV != nullptr)
    {
        // It must never wait for a value that does not come: the driver's own sleep signals it then
        return VulkanHooks::o_vkLatencySleepNV(device, swapchain, pSleepInfo);
    }

    return VK_SUCCESS;
}

void VKAPI_CALL InputReflexVk::SetLatencyMarker(VkDevice device, VkSwapchainKHR swapchain,
                                                const VkSetLatencyMarkerInfoNV* pLatencyMarkerInfo)
{
    if (device == VK_NULL_HANDLE || swapchain == VK_NULL_HANDLE || pLatencyMarkerInfo == nullptr)
        return;

    VulkanHooks::NoteLatencySwapchain(device, swapchain);

    // Out of band markers are ordinary markers here, OptiScaler has no Vulkan frame generation that sends its own
    const auto type = FromVkLatencyMarker(pLatencyMarkerInfo->marker);

    if (!type.has_value())
        return;

    MarkerParams markerParams {};
    markerParams.frame_id = pLatencyMarkerInfo->presentID;
    markerParams.marker_type = *type;

    ReflexHooks::preVkMarker((HANDLE) device, markerParams.frame_id, (uint32_t) markerParams.marker_type,
                             ReflexHooks::isGameCall(_ReturnAddress()));

    LogResult(InputCommon::set_marker(inputContext, (IUnknown*) device, markerParams), "set_marker");
}

void VKAPI_CALL InputReflexVk::GetLatencyTimings(VkDevice device, VkSwapchainKHR swapchain,
                                                 VkGetLatencyMarkerInfoNV* pLatencyMarkerInfo)
{
    if (device == VK_NULL_HANDLE || swapchain == VK_NULL_HANDLE || pLatencyMarkerInfo == nullptr)
        return;

    // The driver's own with the Reflex output on the game's swapchain
    if (auto reflex = InputCommon::reflex_vk_output(); reflex != nullptr &&
                                                       VulkanHooks::LatencySwapchainOf(device).swapchain == swapchain &&
                                                       reflex->get_latency(pLatencyMarkerInfo) == VK_SUCCESS)
    {
        return;
    }

    FrameReport reports[NVAPI_BUFFER_SIZE] {};
    auto result = InputCommon::get_frame_reports(inputContext, (IUnknown*) device, reports);
    LogResult(result, "get_frame_reports");

    // Oldest first, the empty ones are at the start
    uint32_t available = 0;

    if (result == InputResult::Ok)
    {
        for (auto& report : reports)
            available += report.frameID != 0 ? 1 : 0;
    }

    if (pLatencyMarkerInfo->pTimings == nullptr)
    {
        pLatencyMarkerInfo->timingCount = available;
        return;
    }

    // The latest ones
    auto count = std::min(pLatencyMarkerInfo->timingCount, available);
    auto first = NVAPI_BUFFER_SIZE - count;

    for (uint32_t i = 0; i < count; i++)
    {
        auto& in = reports[first + i];
        auto& out = pLatencyMarkerInfo->pTimings[i];

        out.presentID = in.frameID;
        out.inputSampleTimeUs = in.inputSampleTime;
        out.simStartTimeUs = in.simStartTime;
        out.simEndTimeUs = in.simEndTime;
        out.renderSubmitStartTimeUs = in.renderSubmitStartTime;
        out.renderSubmitEndTimeUs = in.renderSubmitEndTime;
        out.presentStartTimeUs = in.presentStartTime;
        out.presentEndTimeUs = in.presentEndTime;
        out.driverStartTimeUs = in.driverStartTime;
        out.driverEndTimeUs = in.driverEndTime;
        out.osRenderQueueStartTimeUs = in.osRenderQueueStartTime;
        out.osRenderQueueEndTimeUs = in.osRenderQueueEndTime;
        out.gpuRenderStartTimeUs = in.gpuRenderStartTime;
        out.gpuRenderEndTimeUs = in.gpuRenderEndTime;
    }

    pLatencyMarkerInfo->timingCount = count;
}

void VKAPI_CALL InputReflexVk::QueueNotifyOutOfBand(VkQueue queue, const VkOutOfBandQueueTypeInfoNV* pQueueTypeInfo)
{
    if (queue == VK_NULL_HANDLE || pQueueTypeInfo == nullptr)
        return;

    // Only the Reflex output on VK_NV_low_latency2 has a use for it
    if (auto reflex = InputCommon::reflex_vk_output())
        reflex->notify_out_of_band(queue, pQueueTypeInfo->queueType);
}
