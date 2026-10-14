#include "pch.h"

#include "input_reflex.h"
#include <hooks/Reflex_Hooks.h>
#include <hooks/Vulkan_Hooks.h>
#include <low_latency/low_latency_tech/ll_reflex_vk.h>
#include <nvapi/fakenvapi.h>

#include <map>

#include <intrin.h>
#include <type_traits>

#pragma intrinsic(_ReturnAddress)

namespace
{
SleepMode ToSleepMode(const NV_SET_SLEEP_MODE_PARAMS& params)
{
    SleepMode sleepMode {};
    sleepMode.low_latency_enabled = params.bLowLatencyMode;
    sleepMode.low_latency_boost = params.bLowLatencyBoost;
    sleepMode.minimum_interval_us = params.minimumIntervalUs;
    sleepMode.use_markers_to_optimize = params.bUseMarkersToOptimize;
    sleepMode.use_min_queue_time = params.bUseMinQueueTime;
    return sleepMode;
}

MarkerParams ToMarker(uint64_t frameId, NV_LATENCY_MARKER_TYPE type)
{
    MarkerParams markerParams {};
    markerParams.frame_id = frameId;
    markerParams.marker_type = (MarkerType) type; // the enums match
    return markerParams;
}

// The game's call was taken: another input's turn, an output that is not up yet or reports that are not there yet are
// no error to the game
NvAPI_Status ToNvApi(InputResult result)
{
    switch (result)
    {
    case InputResult::Ok:
    case InputResult::UsingDifferentInput:
    case InputResult::NotEnoughReports:
    case InputResult::NoReadyOutput:
    case InputResult::InputNotSupported:
        return NVAPI_OK;
    case InputResult::InvalidParameter:
        return NVAPI_INVALID_ARGUMENT;
    default:
        return NVAPI_ERROR;
    }
}

// The game calls these through the pointers ForInterface hands out
static_assert(std::is_same_v<decltype(&InputReflex::D3D_SetSleepMode), decltype(&NvAPI_D3D_SetSleepMode)>);
static_assert(std::is_same_v<decltype(&InputReflex::D3D_Sleep), decltype(&NvAPI_D3D_Sleep)>);
static_assert(std::is_same_v<decltype(&InputReflex::D3D_GetLatency), decltype(&NvAPI_D3D_GetLatency)>);
static_assert(std::is_same_v<decltype(&InputReflex::D3D_SetLatencyMarker), decltype(&NvAPI_D3D_SetLatencyMarker)>);
static_assert(
    std::is_same_v<decltype(&InputReflex::D3D12_SetAsyncFrameMarker), decltype(&NvAPI_D3D12_SetAsyncFrameMarker)>);
static_assert(
    std::is_same_v<decltype(&InputReflex::Vulkan_InitLowLatencyDevice), decltype(&NvAPI_Vulkan_InitLowLatencyDevice)>);
static_assert(std::is_same_v<decltype(&InputReflex::Vulkan_DestroyLowLatencyDevice),
                             decltype(&NvAPI_Vulkan_DestroyLowLatencyDevice)>);
static_assert(std::is_same_v<decltype(&InputReflex::Vulkan_GetSleepStatus), decltype(&NvAPI_Vulkan_GetSleepStatus)>);
static_assert(std::is_same_v<decltype(&InputReflex::Vulkan_SetSleepMode), decltype(&NvAPI_Vulkan_SetSleepMode)>);
static_assert(std::is_same_v<decltype(&InputReflex::Vulkan_Sleep), decltype(&NvAPI_Vulkan_Sleep)>);
static_assert(std::is_same_v<decltype(&InputReflex::Vulkan_GetLatency), decltype(&NvAPI_Vulkan_GetLatency)>);
static_assert(
    std::is_same_v<decltype(&InputReflex::Vulkan_SetLatencyMarker), decltype(&NvAPI_Vulkan_SetLatencyMarker)>);
static_assert(std::is_same_v<decltype(&InputReflex::Vulkan_NotifyOutOfBandVkQueue),
                             decltype(&NvAPI_Vulkan_NotifyOutOfBandVkQueue)>);

// The timeline semaphore each game VkDevice waits on after its sleep (Vulkan_InitLowLatencyDevice)
std::mutex g_vkSemaphoreMutex;
std::map<VkDevice, VkSemaphore> g_vkSemaphores;

VkSemaphore SemaphoreOf(VkDevice device)
{
    std::scoped_lock lock(g_vkSemaphoreMutex);
    auto it = g_vkSemaphores.find(device);
    return it != g_vkSemaphores.end() ? it->second : VK_NULL_HANDLE;
}

void DestroySemaphore(VkDevice device, VkSemaphore semaphore)
{
    if (auto destroy = (PFN_vkDestroySemaphore) VulkanHooks::GetDeviceProcAddr(device, "vkDestroySemaphore"))
        destroy(device, semaphore, nullptr);
}
} // namespace

void* InputReflex::ForInterface(unsigned int interfaceId)
{
    if (interfaceId == GET_ID(NvAPI_D3D_SetSleepMode))
        return &D3D_SetSleepMode;
    if (interfaceId == GET_ID(NvAPI_D3D_Sleep))
        return &D3D_Sleep;
    if (interfaceId == GET_ID(NvAPI_D3D_GetLatency))
        return &D3D_GetLatency;
    if (interfaceId == GET_ID(NvAPI_D3D_SetLatencyMarker))
        return &D3D_SetLatencyMarker;
    if (interfaceId == GET_ID(NvAPI_D3D12_SetAsyncFrameMarker))
        return &D3D12_SetAsyncFrameMarker;
    if (interfaceId == GET_ID(NvAPI_Vulkan_InitLowLatencyDevice))
        return &Vulkan_InitLowLatencyDevice;
    if (interfaceId == GET_ID(NvAPI_Vulkan_DestroyLowLatencyDevice))
        return &Vulkan_DestroyLowLatencyDevice;
    if (interfaceId == GET_ID(NvAPI_Vulkan_GetSleepStatus))
        return &Vulkan_GetSleepStatus;
    if (interfaceId == GET_ID(NvAPI_Vulkan_SetSleepMode))
        return &Vulkan_SetSleepMode;
    if (interfaceId == GET_ID(NvAPI_Vulkan_Sleep))
        return &Vulkan_Sleep;
    if (interfaceId == GET_ID(NvAPI_Vulkan_GetLatency))
        return &Vulkan_GetLatency;
    if (interfaceId == GET_ID(NvAPI_Vulkan_SetLatencyMarker))
        return &Vulkan_SetLatencyMarker;
    if (interfaceId == GET_ID(NvAPI_Vulkan_NotifyOutOfBandVkQueue))
        return &Vulkan_NotifyOutOfBandVkQueue;

    return nullptr;
}

// The game's calls go to InputCommon after the hooks' bookkeeping and game quirks (ReflexHooks::pre*). Calls that are
// not the game's come from the Streamline OptiScaler loads for its DLSS frame generation output: they reach the nvapi
// through the hooks as before (ReflexHooks::route*).

NvAPI_Status InputReflex::D3D_SetSleepMode(IUnknown* pDev, NV_SET_SLEEP_MODE_PARAMS* pSetSleepModeParams)
{
    if (pSetSleepModeParams == nullptr)
        return NVAPI_INVALID_ARGUMENT;

    if (!ReflexHooks::isGameCall(_ReturnAddress()))
        return ReflexHooks::routeSetSleepMode(pDev, pSetSleepModeParams, false);

    if (pDev == nullptr)
        return NVAPI_INVALID_ARGUMENT;

    ReflexHooks::preSetSleepMode(pDev, *pSetSleepModeParams, true);

    auto sleepMode = ToSleepMode(*pSetSleepModeParams);
    return ToNvApi(InputCommon::set_sleep_mode(inputContext, pDev, &sleepMode));
}

NvAPI_Status InputReflex::D3D_Sleep(IUnknown* pDev)
{
    if (!ReflexHooks::isGameCall(_ReturnAddress()))
        return ReflexHooks::routeSleep(pDev, false);

    if (pDev == nullptr)
        return NVAPI_INVALID_ARGUMENT;

    NvAPI_Status result = NVAPI_OK;

    if (ReflexHooks::preSleep(pDev, true, &result) != ReflexHooks::Pre::Deliver)
        return result;

    return ToNvApi(InputCommon::sleep(inputContext, pDev));
}

NvAPI_Status InputReflex::D3D_GetLatency(IUnknown* pDev, NV_LATENCY_RESULT_PARAMS* pGetLatencyParams)
{
    if (!ReflexHooks::isGameCall(_ReturnAddress()))
        return ReflexHooks::routeGetLatency(pDev, pGetLatencyParams);

    if (pDev == nullptr || pGetLatencyParams == nullptr)
        return NVAPI_INVALID_ARGUMENT;

    return ToNvApi(InputCommon::get_latency(inputContext, pDev, pGetLatencyParams));
}

NvAPI_Status InputReflex::GameMarker(IUnknown* pDev, NV_LATENCY_MARKER_PARAMS* params, bool game)
{
    NvAPI_Status result = NVAPI_OK;

    if (ReflexHooks::preMarker(pDev, params, game, &GameMarker, &result) != ReflexHooks::Pre::Deliver)
        return result;

    return ToNvApi(InputCommon::set_marker(inputContext, pDev, ToMarker(params->frameID, params->markerType)));
}

NvAPI_Status InputReflex::D3D_SetLatencyMarker(IUnknown* pDev, NV_LATENCY_MARKER_PARAMS* pSetLatencyMarkerParams)
{
    if (pSetLatencyMarkerParams == nullptr)
        return NVAPI_INVALID_ARGUMENT;

    if (!ReflexHooks::isGameCall(_ReturnAddress()))
        return ReflexHooks::routeSetLatencyMarker(pDev, pSetLatencyMarkerParams, false);

    if (pDev == nullptr)
        return NVAPI_INVALID_ARGUMENT;

    return GameMarker(pDev, pSetLatencyMarkerParams, true);
}

NvAPI_Status InputReflex::D3D12_SetAsyncFrameMarker(ID3D12CommandQueue* pCommandQueue,
                                                    NV_ASYNC_FRAME_MARKER_PARAMS* pSetAsyncFrameMarkerParams)
{
    if (pSetAsyncFrameMarkerParams == nullptr)
        return NVAPI_INVALID_ARGUMENT;

    if (!ReflexHooks::isGameCall(_ReturnAddress()))
        return ReflexHooks::routeSetAsyncFrameMarker(pCommandQueue, pSetAsyncFrameMarkerParams, false);

    ReflexHooks::preAsyncMarker(pSetAsyncFrameMarkerParams->frameID, true);

    return ToNvApi(InputCommon::set_async_marker(
        inputContext, pCommandQueue,
        ToMarker(pSetAsyncFrameMarkerParams->frameID, pSetAsyncFrameMarkerParams->markerType)));
}

// ---- Vulkan: the game's NvAPI_Vulkan_* Reflex ----

// The game gets its own timeline semaphore: the output's sleep doesn't need the driver's NvAPI Vulkan Reflex
NvAPI_Status InputReflex::Vulkan_InitLowLatencyDevice(HANDLE vkDevice, HANDLE* signalSemaphoreHandle)
{
    if (vkDevice == nullptr || signalSemaphoreHandle == nullptr)
        return NVAPI_INVALID_ARGUMENT;

    const auto device = (VkDevice) vkDevice;
    std::scoped_lock lock(g_vkSemaphoreMutex);

    auto& semaphore = g_vkSemaphores[device];

    if (semaphore == VK_NULL_HANDLE)
    {
        VkSemaphoreTypeCreateInfo timelineInfo {};
        timelineInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
        timelineInfo.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
        timelineInfo.initialValue = 0;

        VkSemaphoreCreateInfo createInfo {};
        createInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        createInfo.pNext = &timelineInfo;

        if (VulkanHooks::o_vkCreateSemaphore == nullptr ||
            VulkanHooks::o_vkCreateSemaphore(device, &createInfo, nullptr, &semaphore) != VK_SUCCESS)
        {
            g_vkSemaphores.erase(device);
            LOG_ERROR("Can't create the game's Reflex semaphore");
            return NVAPI_ERROR;
        }

        LOG_INFO("The game's NvAPI Vulkan Reflex is an input: its sleep's semaphore is ours");
    }

    *signalSemaphoreHandle = (HANDLE) semaphore;
    return NVAPI_OK;
}

NvAPI_Status InputReflex::Vulkan_DestroyLowLatencyDevice(HANDLE vkDevice)
{
    if (vkDevice == nullptr)
        return NVAPI_INVALID_ARGUMENT;

    ForgetVulkanDevice((VkDevice) vkDevice);
    return NVAPI_OK;
}

void InputReflex::ForgetVulkanDevice(VkDevice device)
{
    VkSemaphore semaphore = VK_NULL_HANDLE;

    {
        std::scoped_lock lock(g_vkSemaphoreMutex);

        if (auto it = g_vkSemaphores.find(device); it != g_vkSemaphores.end())
        {
            semaphore = it->second;
            g_vkSemaphores.erase(it);
        }
    }

    if (semaphore != VK_NULL_HANDLE)
        DestroySemaphore(device, semaphore);
}

NvAPI_Status InputReflex::Vulkan_GetSleepStatus(HANDLE vkDevice,
                                                NV_VULKAN_GET_SLEEP_STATUS_PARAMS* pGetSleepStatusParams)
{
    if (vkDevice == nullptr || pGetSleepStatusParams == nullptr)
        return NVAPI_INVALID_ARGUMENT;

    SleepParams sleepParams {};
    auto result = InputCommon::get_sleep_status(vulkanContext, (IUnknown*) vkDevice, &sleepParams);
    pGetSleepStatusParams->bLowLatencyMode = sleepParams.low_latency_enabled;
    return ToNvApi(result);
}

NvAPI_Status InputReflex::Vulkan_SetSleepMode(HANDLE vkDevice, NV_VULKAN_SET_SLEEP_MODE_PARAMS* pSetSleepModeParams)
{
    if (vkDevice == nullptr || pSetSleepModeParams == nullptr)
        return NVAPI_INVALID_ARGUMENT;

    ReflexHooks::preVkSetSleepMode(vkDevice, pSetSleepModeParams->bLowLatencyMode != 0,
                                   ReflexHooks::isGameCall(_ReturnAddress()));

    SleepMode sleepMode {};
    sleepMode.low_latency_enabled = pSetSleepModeParams->bLowLatencyMode;
    sleepMode.low_latency_boost = pSetSleepModeParams->bLowLatencyBoost;
    sleepMode.minimum_interval_us = pSetSleepModeParams->minimumIntervalUs;
    sleepMode.use_markers_to_optimize = true;

    return ToNvApi(InputCommon::set_sleep_mode(vulkanContext, (IUnknown*) vkDevice, &sleepMode));
}

NvAPI_Status InputReflex::Vulkan_Sleep(HANDLE vkDevice, NvU64 signalValue)
{
    if (vkDevice == nullptr)
        return NVAPI_INVALID_ARGUMENT;

    const auto device = (VkDevice) vkDevice;
    const auto semaphore = SemaphoreOf(device);

    // The game made its low latency device before our hooks: its semaphore is the driver's, and only the driver's Sleep
    // signals it (the game would wait for a value that never comes). Its mode went to InputCommon's output as usual.
    // fakenvapi's Sleep is this one (nvapi_calls.cpp): nothing else signals there.
    if (semaphore == VK_NULL_HANDLE)
    {
        static std::atomic_bool logged = false;

        if (fakenvapi::isUsingAsMainNvapi())
            return NVAPI_OK;

        if (!logged.exchange(true))
            LOG_WARN("The game's NvAPI Vulkan Reflex semaphore is the driver's: its Sleep goes to the driver");

        return ReflexHooks::nvapiVkSleep(vkDevice, signalValue);
    }

    ReflexHooks::preVkSleep(vkDevice, ReflexHooks::isGameCall(_ReturnAddress()));
    auto result = InputCommon::sleep(vulkanContext, (IUnknown*) vkDevice);

    // The game waits for it whatever the output did (another input's turn, no output yet)
    VulkanHooks::SignalTimelineSemaphore(device, semaphore, signalValue);

    return ToNvApi(result);
}

NvAPI_Status InputReflex::Vulkan_GetLatency(HANDLE vkDevice, NV_VULKAN_LATENCY_RESULT_PARAMS* pGetLatencyParams)
{
    if (vkDevice == nullptr || pGetLatencyParams == nullptr)
        return NVAPI_INVALID_ARGUMENT;

    if (pGetLatencyParams->version != NV_VULKAN_LATENCY_RESULT_PARAMS_VER1)
    {
        LOG_ERROR("Unsupported version {}", pGetLatencyParams->version);
        return NVAPI_INCOMPATIBLE_STRUCT_VERSION;
    }

    // The D3D report (the driver's with a Reflex output, else the markers'), copied field by field into the Vulkan
    // layout. Static and big: not freed, like the others.
    static std::mutex reportMutex;
    static NV_LATENCY_RESULT_PARAMS* report = new NV_LATENCY_RESULT_PARAMS();

    std::scoped_lock lock(reportMutex);
    *report = {};
    report->version = NV_LATENCY_RESULT_PARAMS_VER;

    auto result = InputCommon::get_latency(vulkanContext, (IUnknown*) vkDevice, report);

    for (size_t i = 0; i < std::size(report->frameReport); i++)
    {
        const auto& from = report->frameReport[i];
        auto& to = pGetLatencyParams->frameReport[i];

        to = {};
        to.frameID = from.frameID;
        to.inputSampleTime = from.inputSampleTime;
        to.simStartTime = from.simStartTime;
        to.simEndTime = from.simEndTime;
        to.renderSubmitStartTime = from.renderSubmitStartTime;
        to.renderSubmitEndTime = from.renderSubmitEndTime;
        to.presentStartTime = from.presentStartTime;
        to.presentEndTime = from.presentEndTime;
        to.driverStartTime = from.driverStartTime;
        to.driverEndTime = from.driverEndTime;
        to.osRenderQueueStartTime = from.osRenderQueueStartTime;
        to.osRenderQueueEndTime = from.osRenderQueueEndTime;
        to.gpuRenderStartTime = from.gpuRenderStartTime;
        to.gpuRenderEndTime = from.gpuRenderEndTime;
    }

    return ToNvApi(result);
}

NvAPI_Status InputReflex::Vulkan_SetLatencyMarker(HANDLE vkDevice,
                                                  NV_VULKAN_LATENCY_MARKER_PARAMS* pSetLatencyMarkerParams)
{
    if (vkDevice == nullptr || pSetLatencyMarkerParams == nullptr)
        return NVAPI_INVALID_ARGUMENT;

    const auto frameId = pSetLatencyMarkerParams->frameID;
    const auto type = (uint32_t) pSetLatencyMarkerParams->markerType; // NVAPI's D3D and Vulkan ordinals match

    ReflexHooks::preVkMarker(vkDevice, frameId, type, ReflexHooks::isGameCall(_ReturnAddress()));

    MarkerParams markerParams {};
    markerParams.frame_id = frameId;
    markerParams.marker_type = (MarkerType) type;

    return ToNvApi(InputCommon::set_marker(vulkanContext, (IUnknown*) vkDevice, markerParams));
}

NvAPI_Status InputReflex::Vulkan_NotifyOutOfBandVkQueue(HANDLE vkDevice, HANDLE queueHandle,
                                                        NV_VULKAN_OUT_OF_BAND_QUEUE_TYPE queueType)
{
    if (vkDevice == nullptr || queueHandle == nullptr)
        return NVAPI_INVALID_ARGUMENT;

    // Only the VK_NV_low_latency2 output has a use for it, the queue types match
    if (auto reflex = InputCommon::reflex_vk_output())
        reflex->notify_out_of_band((VkQueue) queueHandle, (VkOutOfBandQueueTypeNV) queueType);

    return NVAPI_OK;
}
