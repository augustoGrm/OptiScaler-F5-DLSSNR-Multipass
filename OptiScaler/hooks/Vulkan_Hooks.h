#pragma once
#include "SysUtils.h"

class VulkanHooks
{
    inline static std::atomic<VkSwapchainKHR> lowLatencySwapchain = VK_NULL_HANDLE;

  public:
    static PFN_vkCreateSemaphore o_vkCreateSemaphore;
    static PFN_vkSignalSemaphore o_vkSignalSemaphore;
    static PFN_vkAntiLagUpdateAMD o_vkAntiLagUpdateAMD;

    // The device was created with the timeline semaphore feature on (the game's own, or switched on for Optical F5Low).
    static bool TimelineSemaphoresOn(VkDevice device);

    // vkWaitSemaphores on one timeline semaphore for one value; true once it is reached, false on timeout or when the
    // call is not there.
    static bool WaitTimelineSemaphore(VkDevice device, VkSemaphore semaphore, uint64_t value, uint64_t timeoutNs);

    // vkSignalSemaphore on one timeline semaphore, from the host; true when it went through.
    static bool SignalTimelineSemaphore(VkDevice device, VkSemaphore semaphore, uint64_t value);

    // vkCreateWin32SurfaceKHR without our hook (the present bridge makes a surface of its own).
    static PFN_vkCreateWin32SurfaceKHR OriginalCreateWin32Surface();

    // vkDestroySurfaceKHR without our hook: the bridge destroys that surface holding its own lock, which the hook
    // takes.
    static PFN_vkDestroySurfaceKHR OriginalDestroySurface();

    // The driver's VK_NV_low_latency2
    inline static PFN_vkSetLatencySleepModeNV o_vkSetLatencySleepModeNV = nullptr;
    inline static PFN_vkLatencySleepNV o_vkLatencySleepNV = nullptr;
    inline static PFN_vkSetLatencyMarkerNV o_vkSetLatencyMarkerNV = nullptr;
    inline static PFN_vkGetLatencyTimingsNV o_vkGetLatencyTimingsNV = nullptr;
    inline static PFN_vkQueueNotifyOutOfBandNV o_vkQueueNotifyOutOfBandNV = nullptr;

    // The swapchain with VK_NV_low_latency2's low latency mode, for the Reflex output
    static VkSwapchainKHR LowLatencySwapchain() { return lowLatencySwapchain; }
    static void SetLowLatencySwapchain(VkSwapchainKHR swapchain);
    static void ForgetLowLatencySwapchain(VkSwapchainKHR swapchain)
    {
        lowLatencySwapchain.compare_exchange_strong(swapchain, VK_NULL_HANDLE);
    }

    // vkGetDeviceProcAddr without OptiScaler's hooks
    static PFN_vkVoidFunction GetDeviceProcAddr(VkDevice device, const char* pName);

    static void Hook(HMODULE vulkan1);
    static void Unhook();
};
