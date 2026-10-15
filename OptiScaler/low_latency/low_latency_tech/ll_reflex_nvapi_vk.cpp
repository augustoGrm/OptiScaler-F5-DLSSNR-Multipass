#include "pch.h"
#include "ll_reflex_nvapi_vk.h"

#include "ll_reflex.h"

#include <hooks/Reflex_Hooks.h>
#include <hooks/Vulkan_Hooks.h>

#include <atomic>
#include <map>
#include <memory>
#include <mutex>

namespace
{
using native::lowlatency::VkSleepCalls;
using native::lowlatency::VkSleepState;
using native::lowlatency::VkSleepSync;
using native::lowlatency::VkSleepWhy;

// Per device, kept across outputs (forget_device). Each its own lock: a sleep (the driver's and the wait for its value)
// holds only its device's, the map's lock only finds it.
struct DeviceSync
{
    std::mutex mutex;
    VkSleepSync sync;
};

std::mutex g_syncsMutex;
std::map<VkDevice, std::shared_ptr<DeviceSync>> g_syncs;

std::shared_ptr<DeviceSync> SyncOf(VkDevice device, bool create)
{
    std::scoped_lock lock(g_syncsMutex);

    if (auto it = g_syncs.find(device); it != g_syncs.end())
        return it->second;

    if (!create)
        return nullptr;

    auto sync = std::make_shared<DeviceSync>();
    g_syncs[device] = sync;
    return sync;
}

VkSleepCalls SleepCalls(VkDevice device)
{
    VkSleepCalls calls;
    calls.timelineSemaphoresOn = [device] { return VulkanHooks::TimelineSemaphoresOn(device); };
    calls.init = [device](void** semaphore)
    {
        HANDLE handle = nullptr;
        const auto status = ReflexHooks::nvapiVkInitLowLatencyDevice(reinterpret_cast<HANDLE>(device), &handle);
        *semaphore = handle;
        return static_cast<int>(status);
    };
    calls.sleep = [device](uint64_t value)
    { return static_cast<int>(ReflexHooks::nvapiVkSleep(reinterpret_cast<HANDLE>(device), value)); };
    calls.wait = [device](void* semaphore, uint64_t value, uint64_t timeoutNs)
    { return VulkanHooks::WaitTimelineSemaphore(device, reinterpret_cast<VkSemaphore>(semaphore), value, timeoutNs); };
    return calls;
}

const char* WhyText(VkSleepWhy why)
{
    switch (why)
    {
    case VkSleepWhy::NoTimelineSemaphores:
        return "the device was made without timeline semaphores";
    case VkSleepWhy::InitFailed:
        return "NvAPI_Vulkan_InitLowLatencyDevice failed";
    case VkSleepWhy::NoSemaphore:
        return "NvAPI_Vulkan_InitLowLatencyDevice returned no semaphore";
    case VkSleepWhy::WaitTimedOut:
        return "the sleep's semaphore never reached its value";
    default:
        return "";
    }
}
} // namespace

bool ReflexNvApiVk::init(IUnknown* pDevice)
{
    const auto vkDevice = reinterpret_cast<VkDevice>(pDevice);

    if (vkDevice == VK_NULL_HANDLE)
        return false;

    if (!ReflexDriverReady() || !ReflexHooks::nvapiVulkanReady())
    {
        LOG_INFO("Reflex Vulkan (NvAPI) needs NVIDIA's nvapi with its Vulkan Reflex");
        return false;
    }

    auto entry = SyncOf(vkDevice, true);
    std::scoped_lock lock(entry->mutex);

    if (!native::lowlatency::VkSleepReady(entry->sync, SleepCalls(vkDevice)))
    {
        LOG_INFO("Reflex Vulkan (NvAPI) can't start on this device: {} (NvAPI status {})", WhyText(entry->sync.why),
                 entry->sync.initStatus);
        return false;
    }

    device = vkDevice;
    LOG_INFO("Reflex Vulkan (NvAPI) initialized");
    return true;
}

void ReflexNvApiVk::deinit()
{
    // Leave the driver without low latency for the next output
    if (device != VK_NULL_HANDLE)
    {
        NV_VULKAN_SET_SLEEP_MODE_PARAMS params {};
        params.version = NV_VULKAN_SET_SLEEP_MODE_PARAMS_VER;
        ReflexHooks::nvapiVkSetSleepMode(reinterpret_cast<HANDLE>(device), &params);
    }

    device = VK_NULL_HANDLE;
    LOG_INFO("Reflex Vulkan (NvAPI) deinitialized");
}

void ReflexNvApiVk::forget_device(VkDevice device)
{
    std::shared_ptr<DeviceSync> entry;
    {
        std::scoped_lock lock(g_syncsMutex);

        if (auto it = g_syncs.find(device); it != g_syncs.end())
        {
            entry = std::move(it->second);
            g_syncs.erase(it);
        }
    }

    if (entry == nullptr)
        return;

    // After a sleep under way; the driver's low latency state goes before the device (NVAPI's order)
    std::scoped_lock lock(entry->mutex);

    if (entry->sync.state == VkSleepState::Ready)
        ReflexHooks::nvapiVkDestroyLowLatencyDevice(reinterpret_cast<HANDLE>(device));
}

bool ReflexNvApiVk::is_enabled()
{
    auto force = RealReflexForceState();
    return force != ForceReflex::InGame ? force == ForceReflex::ForceEnable : low_latency_enabled;
}

void ReflexNvApiVk::apply_sleep_mode()
{
    if (device == VK_NULL_HANDLE || !sleep_mode_known)
        return;

    NV_VULKAN_SET_SLEEP_MODE_PARAMS params {};
    params.version = NV_VULKAN_SET_SLEEP_MODE_PARAMS_VER;
    params.bLowLatencyMode = is_enabled();
    params.bLowLatencyBoost = sleep_mode.low_latency_boost;
    params.minimumIntervalUs = sleep_mode.minimum_interval_us;

    auto result = ReflexHooks::nvapiVkSetSleepMode(reinterpret_cast<HANDLE>(device), &params);
    applied_enabled = params.bLowLatencyMode;

    LOG_INFO("Reflex Vulkan (NvAPI) sleep mode, low latency: {}, boost: {}, interval: {} us, result: {}",
             (int) params.bLowLatencyMode, (int) params.bLowLatencyBoost, params.minimumIntervalUs, (int) result);
}

void ReflexNvApiVk::get_sleep_status(SleepParams* sleep_params)
{
    NV_VULKAN_GET_SLEEP_STATUS_PARAMS status {};
    status.version = NV_VULKAN_GET_SLEEP_STATUS_PARAMS_VER;

    if (device == VK_NULL_HANDLE ||
        ReflexHooks::nvapiVkGetSleepStatus(reinterpret_cast<HANDLE>(device), &status) != NVAPI_OK)
    {
        sleep_params->low_latency_enabled = is_enabled();
        return;
    }

    sleep_params->low_latency_enabled = status.bLowLatencyMode;
}

void ReflexNvApiVk::set_sleep_mode(SleepMode* sleep_mode)
{
    // Some games set the same sleep mode every frame
    if (sleep_mode_known && is_enabled() == applied_enabled && *sleep_mode == this->sleep_mode)
        return;

    low_latency_enabled = sleep_mode->low_latency_enabled;
    this->sleep_mode = *sleep_mode;
    sleep_mode_known = true;
    apply_sleep_mode();
}

void ReflexNvApiVk::sleep(std::optional<uint32_t> frame_id)
{
    if (device == VK_NULL_HANDLE)
        return;

    // Force State in the menu can change at any time
    if (sleep_mode_known && is_enabled() != applied_enabled)
        apply_sleep_mode();

    auto entry = SyncOf(device, false);

    if (entry == nullptr)
        return;

    std::scoped_lock lock(entry->mutex);

    auto& sync = entry->sync;
    bool firstTimeout = false;
    native::lowlatency::VkSleepFrame(sync, SleepCalls(device), &firstTimeout);

    if (firstTimeout)
    {
        LOG_WARN("Reflex Vulkan (NvAPI): the sleep's semaphore did not reach its value within {} ms",
                 native::lowlatency::kVkSleepWaitNs / 1000000);
    }

    // A driver that never signals: no more sleeps on this device (NativeLowLatencyVkSync.h), once said
    static std::atomic<VkDevice> unavailableLogged = VK_NULL_HANDLE;

    if (sync.state == VkSleepState::Unavailable && unavailableLogged.exchange(device) != device)
        LOG_WARN("Reflex Vulkan (NvAPI): no more sleeps on this device ({})", WhyText(sync.why));
}

void ReflexNvApiVk::set_marker(IUnknown* pDevice, const MarkerParams& marker_params)
{
    if (device == VK_NULL_HANDLE)
        return;

    NV_VULKAN_LATENCY_MARKER_PARAMS params {};
    params.version = NV_VULKAN_LATENCY_MARKER_PARAMS_VER;
    params.frameID = marker_params.frame_id;
    params.markerType = (NV_VULKAN_LATENCY_MARKER_TYPE) marker_params.marker_type; // the enums match

    ReflexHooks::nvapiVkSetLatencyMarker(reinterpret_cast<HANDLE>(device), &params);
}

void ReflexNvApiVk::set_async_marker(IUnknown* pCommandQueue, const MarkerParams& marker_params)
{
    // Out of band markers are ordinary markers on the device here; the queue (a D3D12 one next to XeFG) isn't Vulkan's
    set_marker(nullptr, marker_params);
}

NvAPI_Status ReflexNvApiVk::get_latency(NV_VULKAN_LATENCY_RESULT_PARAMS* latency_params) const
{
    if (device == VK_NULL_HANDLE)
        return NVAPI_INVALID_ARGUMENT;

    return ReflexHooks::nvapiVkGetLatency(reinterpret_cast<HANDLE>(device), latency_params);
}

void ReflexNvApiVk::notify_out_of_band(HANDLE queue, NV_VULKAN_OUT_OF_BAND_QUEUE_TYPE queue_type) const
{
    if (device != VK_NULL_HANDLE && queue != nullptr)
        ReflexHooks::nvapiVkNotifyOutOfBandVkQueue(reinterpret_cast<HANDLE>(device), queue, queue_type);
}
