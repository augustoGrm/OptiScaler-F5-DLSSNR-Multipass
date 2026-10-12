#pragma once

#include "low_latency/low_latency_tech/low_latency_tech.h"

#include <dxgi.h>
#include <d3d12.h>

#include "low_latency/ll_util.h"
#include <optional>

class LowLatency
{
  private:
    std::atomic<std::shared_ptr<LowLatencyTech>> currently_active_tech;
    FrameReport frame_reports[FRAME_REPORTS_BUFFER_SIZE] {};
    std::optional<bool> forced_fg;
    bool fg;
    uint32_t delay_deinit = 0;
    SleepMode last_sleep_mode {};

    // Once set, it will be used for all init attempts
    LowLatencyMode forced_low_latency_tech = LowLatencyMode::None;

    // A context that runs Anti-Lag 2 (D3D) or Anti-Lag Vk (Vulkan) and nothing else (use_only): its tech could not start.
    // Read from other threads (fakenvapi::companionFailed).
    std::atomic<bool> forced_init_failed = false;

    // Simulation starts of the last two frames, for the report's GPU frame time: per context (two contexts can run at
    // once with XeFG's Anti-Lag 2 output)
    uint64_t last_sim_start = 0;
    uint64_t second_last_sim_start = 0;

    void update_effective_fg_state();
    void update_enabled_override();

    // D3D
    bool update_low_latency_tech(IUnknown* pDevice);
    void get_latency_result(NV_LATENCY_RESULT_PARAMS* pGetLatencyParams);
    void add_marker_to_report(NV_LATENCY_MARKER_PARAMS* pSetLatencyMarkerParams);

    // Vulkan
    bool update_low_latency_tech(HANDLE vkDevice);
    void get_latency_result(NV_VULKAN_LATENCY_RESULT_PARAMS* pGetLatencyParams);
    void add_marker_to_report(NV_VULKAN_LATENCY_MARKER_PARAMS* pSetLatencyMarkerParams);

  public:
    LowLatency() = default;
    ~LowLatency() { deinit_current_tech(); };

    bool deinit_current_tech();
    void set_forced_fg(std::optional<bool> forced_fg) { this->forced_fg = forced_fg; };
    void set_fg_type(bool interpolated, uint64_t frame_id)
    {
        if (auto current_tech = currently_active_tech.load())
        {
            if (current_tech)
                current_tech->set_fg_type(interpolated, frame_id);
        }
    }
    // For a second context next to the main one (fakenvapi::companion*): it runs `tech` (AntiLag2 on D3D, AntiLagVk on
    // Vulkan) and nothing else, never falling back to another, and does not try again once the tech could not start. The
    // tech is made by the first call that brings a device. A tech it already has is let go.
    void use_only(LowLatencyMode tech);
    bool init_failed() const { return forced_init_failed.load(); }
    bool get_low_latency_tech_context(void** low_latency_tech_context, LowLatencyMode* low_latency_tech);
    bool set_low_latency_tech_mode(IUnknown* device, LowLatencyMode low_latency_tech);

    bool is_low_latency_enabled();

    // D3D
    NvAPI_Status Sleep(IUnknown* pDevice);
    NvAPI_Status SetSleepMode(IUnknown* pDevice, NV_SET_SLEEP_MODE_PARAMS* pSetSleepModeParams);
    NvAPI_Status GetSleepStatus(IUnknown* pDevice, NV_GET_SLEEP_STATUS_PARAMS* pGetSleepStatusParams);
    NvAPI_Status SetLatencyMarker(IUnknown* pDev, NV_LATENCY_MARKER_PARAMS* pSetLatencyMarkerParams);
    NvAPI_Status SetAsyncFrameMarker(ID3D12CommandQueue* pCommandQueue,
                                     NV_ASYNC_FRAME_MARKER_PARAMS* pSetAsyncFrameMarkerParams);
    NvAPI_Status GetLatency(IUnknown* pDev, NV_LATENCY_RESULT_PARAMS* pGetLatencyParams);

    // Vulkan
    NvAPI_Status Sleep(HANDLE vkDevice);
    NvAPI_Status SetSleepMode(HANDLE vkDevice, NV_VULKAN_SET_SLEEP_MODE_PARAMS* pSetSleepModeParams);
    NvAPI_Status GetSleepStatus(HANDLE vkDevice, NV_VULKAN_GET_SLEEP_STATUS_PARAMS* pGetSleepStatusParams);
    NvAPI_Status SetLatencyMarker(HANDLE vkDevice, NV_VULKAN_LATENCY_MARKER_PARAMS* pSetLatencyMarkerParams);
    NvAPI_Status GetLatency(HANDLE vkDevice, NV_VULKAN_LATENCY_RESULT_PARAMS* pGetLatencyParams);
};

// This is a wrapper for the entire LowLatency abstraction layer
// If you need a specific *tech* context then you need to query this abstraction layer
class LowLatencyCtx
{
  public:
    static void init()
    {
        if (!lowlatency_ctx)
            lowlatency_ctx = new LowLatency();
    }

    static void shutdown()
    {
        delete lowlatency_ctx;
        lowlatency_ctx = nullptr;
    }

    static LowLatency* get() { return lowlatency_ctx; }

  private:
    static LowLatency* lowlatency_ctx;
};
