#pragma once

#include <dxgi.h>
#include <d3d12.h>
#include <atomic>
#include <unordered_map>
#include "low_latency/ll_util.h"

class fakenvapi
{
    inline static struct AntiLag2Data
    {
        void* context;
        bool enabled;
    } antilag2_data;

    inline static bool _usingFakenvapiAsMainNvapi = false;
    inline static bool _usingOnNvidia = false;
    inline static void* _lowLatencyTechContext = nullptr;
    inline static LowLatencyMode _lowLatencyMode = LowLatencyMode::LatencyFlex;
    inline static HMODULE _dllForNvidia = nullptr;
    inline static std::atomic<uint32_t> _forcedModeGeneration = 0;

    static std::unordered_map<NvU32, void*> idToFuncMapping;

    static bool updateModeAndContext();

    static NvAPI_Status __cdecl placeholder()
    {
        // return OK();
        // return ERROR(NVAPI_NO_IMPLEMENTATION);
        return NVAPI_NO_IMPLEMENTATION; // no logging
    }

  public:
    inline static const GUID IID_IFfxAntiLag2Data = {
        0x5083ae5b, 0x8070, 0x4fca, { 0x8e, 0xe5, 0x35, 0x82, 0xdd, 0x36, 0x7d, 0x13 }
    };

    static void init(bool onlyContext);
    static void deinit();
    static void* queryInterface(NvU32 id);
    static void reportFGPresent(IDXGISwapChain* pSwapChain, bool fg_state, bool frame_interpolated);
    static bool forceMode(IUnknown* device, LowLatencyMode mode);
    // Counts forceMode calls: each can destroy the current low latency context and make a new one, which may land at the
    // old one's address, so a context is told apart by this rather than by its pointer
    static uint32_t forcedModeGeneration();
    static bool isLowLatencyActive();

    static LowLatencyMode getCurrentMode();
    static void* getCurrentContext();

    static bool isUsingAsMainNvapi();
    static void setUsingAsMainNvapi(bool usingAsMain);

    // XeFG with Anti-Lag 2 as the low latency Output (native/XefgReflexRule.h): the main context is XeLL's (XeFG forces
    // it), and Anti-Lag 2 (D3D) or Anti-Lag Vk (Vulkan) runs in a second context of its own beside it, one tech each,
    // made by the first call that brings a device. A call returns an error when that tech cannot start (no AMD card,
    // no driver support): the caller then treats its mode as refused. Any thread.
    static NvAPI_Status companionSetSleepMode(IUnknown* device, NV_SET_SLEEP_MODE_PARAMS* params);
    static NvAPI_Status companionSleep(IUnknown* device);
    static NvAPI_Status companionSetLatencyMarker(IUnknown* device, NV_LATENCY_MARKER_PARAMS* params);
    static NvAPI_Status companionSetAsyncFrameMarker(ID3D12CommandQueue* queue, NV_ASYNC_FRAME_MARKER_PARAMS* params);
    static NvAPI_Status companionGetLatency(IUnknown* device, NV_LATENCY_RESULT_PARAMS* params);
    static NvAPI_Status companionSetSleepMode(HANDLE vkDevice, NV_VULKAN_SET_SLEEP_MODE_PARAMS* params);
    static NvAPI_Status companionSetLatencyMarker(HANDLE vkDevice, NV_VULKAN_LATENCY_MARKER_PARAMS* params);
    static NvAPI_Status companionGetLatency(HANDLE vkDevice, NV_VULKAN_LATENCY_RESULT_PARAMS* params);
    // The device is gone: its second context lets its tech go (a new device makes it again)
    static void companionDeviceGone(const void* device);
    // The second context's tech could not start (no AMD card, no driver support): set by the first call that finds out and
    // kept for the session, so the Output falls back to XeLL and stays there. Any thread.
    static bool companionFailed();
    // The second contexts are let go (the Output went back to XeLL): Anti-Lag 2 does not stay initialised beside it
    static void companionRelease();
};
