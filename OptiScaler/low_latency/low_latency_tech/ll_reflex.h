#pragma once

#include "low_latency_tech.h"

// Ignores volatile ForceDisable
inline ForceReflex RealReflexForceState()
{
    auto& force = Config::Instance()->FN_ForceReflex;

    if (force.value_or_default() == ForceReflex::ForceDisable && !force.value_for_config().has_value())
        return ForceReflex::InGame;

    return force.value_or_default();
}

// NVIDIA Reflex as an output: every call goes to the driver past the hooks (ReflexHooks::nvapi*), the hooks only see
// the inputs' calls. InputCommon gives it the sleep mode on OptiScaler's terms (OutputRule.h SleepModeFor).
class Reflex : public LowLatencyTech
{
    IUnknown* device = nullptr;
    SleepMode sleep_mode {};
    bool sleep_mode_known = false;
    bool applied_enabled = false;

    decltype(&NvAPI_D3D12_SetAsyncFrameMarker) o_SetAsyncFrameMarker = nullptr;
    decltype(&NvAPI_D3D_GetSleepStatus) o_GetSleepStatus = nullptr;
    decltype(&NvAPI_D3D_SetReflexSync) o_SetReflexSync = nullptr;

    void apply_sleep_mode();

  public:
    Reflex() : LowLatencyTech() {}

    // From LowLatencyTech
    bool init(IUnknown* pDevice) override;
    void deinit() override;

    LowLatencyMode get_mode() override { return LowLatencyMode::Reflex; };
    void* get_tech_context() override { return device; };
    void set_fg_type(bool interpolated, uint64_t frame_id) override {}; // Async markers carry this for Reflex
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

    // For passthrough
    NvAPI_Status get_latency(NV_LATENCY_RESULT_PARAMS* latency_params) const;

    // The frame generation multiplier DLSSG reports every frame
    void set_reflex_sync(uint32_t frameMultiplier) const;

    // Reflex Sync is there (R530+), with the multiplier the driver applies the FPS limit to the output frames
    bool has_reflex_sync() const { return o_SetReflexSync != nullptr && device != nullptr; }
};
