#include "pch.h"

// Upstream's low latency input layer, on OptiScaler's terms (OutputRule.h). Without LOW_LATENCY_INPUTS its inputs are
// the game's D3D Reflex (input_reflex.cpp), OptiScaler's own low latency (native/NativeLowLatency.cpp) and the UE Low
// Latency mod (input_uell.cpp); its outputs Reflex, Anti-Lag 2, XeLL (also XeFG's) and LatencyFlex. The game's XeLL
// and Vulkan Reflex stay on our own hooks, and there is no Vulkan output yet.

#include <misc/IdentifyGpu.h>

#include "input_common.h"
#include "input_xell.h"
#include <low_latency/low_latency_tech/ll_xell.h>
#include <low_latency/low_latency_tech/ll_antilag2.h>
#include <low_latency/low_latency_tech/ll_latencyflex.h>
#include <low_latency/low_latency_tech/ll_reflex.h>
#include <low_latency/low_latency_tech/ll_reflex_vk.h>
#include <low_latency/low_latency_tech/ll_antilag_vk.h>
#include <low_latency/low_latency_tech/ll_fps_limiter.h>
#include <hooks/Vulkan_Hooks.h>
#ifdef LOW_LATENCY_INPUTS
#include <inputs/FG/XeFG_Inputs_Dx12.h>
#endif
#include <framegen/IFGFeature_Dx12.h>
#include <nvapi/fakenvapi.h>
#include "OutputRule.h"
#include <native/NativeLowLatency.h>
#include <hooks/Reflex_Hooks.h>

#include <deque>

static bool marker_supported(InputMarkerMode mode, MarkerType type)
{
    switch (mode)
    {
    case InputMarkerMode::NoMarkers:
        return false;
    case InputMarkerMode::SimStartOnly:
        return type == MarkerType::SIMULATION_START;
    case InputMarkerMode::SimStartAndPresentStart:
        return type == MarkerType::SIMULATION_START || type == MarkerType::PRESENT_START ||
               type == MarkerType::OUT_OF_BAND_PRESENT_START;
    case InputMarkerMode::SimOnly:
        return type == MarkerType::SIMULATION_START || type == MarkerType::SIMULATION_END;
    default:
        return true;
    }
}

// private
bool InputCommon::deinit_current_tech()
{
    // currently_active_tech becomes nullptr
    // but we need to wait for all users of the old one to release
    auto old_tech = currently_active_tech.exchange(nullptr);

    if (old_tech)
    {
        LOG_TRACE("Deiniting current tech");

        // XeFG's XeLL stays XeFG's: it only stops being the output (and gets its low latency off next to the new one)
        if (old_tech != xefg_xell.load())
        {
            while (old_tech.use_count() > 1)
                std::this_thread::yield();

            old_tech->deinit();
        }

        output_device = nullptr;
        clear_reports();

        return true;
    }

    return false;
}

bool InputCommon::init_tech(IUnknown* pDevice, API api, LowLatencyMode desiredMode, const lloutput::OutputPlan& plan)
{
    if (currently_active_tech.load() || delay_deinit != 0)
        return false;

    auto make = [](LowLatencyMode mode) -> std::shared_ptr<LowLatencyTech>
    {
        switch (mode)
        {
        case LowLatencyMode::XeLL:
            return std::make_shared<XeLL>();
        case LowLatencyMode::AntiLag2:
            return std::make_shared<AntiLag2>();
        case LowLatencyMode::Reflex:
            return std::make_shared<Reflex>();
        case LowLatencyMode::AntiLagVk:
            return std::make_shared<AntiLagVk>();
        case LowLatencyMode::LatencyFlex:
            return std::make_shared<LatencyFlex>();
        case LowLatencyMode::FpsLimiter:
            return std::make_shared<FpsLimiter>();
        default:
            return nullptr;
        }
    };

    // The plan's outputs in order when the plan's first one is wanted (fakenvapi's Anti-Lag 2, XeLL, LatencyFlex)
    std::array<LowLatencyMode, 3> tries { desiredMode, LowLatencyMode::None, LowLatencyMode::None };

    if (desiredMode == to_mode(plan.first()))
    {
        for (size_t i = 0; i < tries.size(); i++)
            tries[i] = to_mode(plan.tries[i]);
    }

    std::shared_ptr<LowLatencyTech> tech;

    for (auto mode : tries)
    {
        if (mode == LowLatencyMode::None)
            continue;

        if (auto candidate = make(mode); candidate != nullptr && candidate->init(pDevice))
        {
            LOG_INFO("LowLatency algo: {}", magic_enum::enum_name(mode));
            tech = std::move(candidate);
            break;
        }

        LOG_INFO("LowLatency algo {} could not start", magic_enum::enum_name(mode));
    }

    if (tech == nullptr)
    {
        failed_output = desiredMode;
        failed_time = std::chrono::steady_clock::now().time_since_epoch().count();

        // With XeFG the output is XeFG's choice: no stand-in next to its XeLL. Anti-Lag 2 that can't start hands the
        // split back to XeLL (native::lowlatency::XefgSide).
        if (plan.xefgOwnXell)
        {
            if (desiredMode == LowLatencyMode::AntiLag2 && !antilag2_failed.exchange(true))
                LOG_WARN("Anti-Lag 2 could not start next to XeFG's XeLL, XeLL lowers latency");

            return false;
        }

        if (desiredMode != LowLatencyMode::LatencyFlex)
        {
            tech = make(LowLatencyMode::LatencyFlex);

            if (!tech->init(pDevice))
                return false;

            LOG_INFO("LowLatency algo: LatencyFlex (Fallback)");
        }
        else
        {
            return false;
        }
    }

    currently_active_tech.store(tech);
    activeOutput = tech->get_mode();
    output_api = api;
    output_device = pDevice;

    // XeFG needs XeLL in low latency mode, disabling it only stops sending the sleep and marker calls
    tech->set_forced_mode(xefg_paced());
    tech->set_low_latency_override(Config::Instance()->FN_ForceReflex.value_or_default());

    apply_sleep_mode(tech.get()); // Restore any potential sleep mode
    return true;
}

LowLatencyMode InputCommon::to_mode(lloutput::Tech tech)
{
    switch (tech)
    {
    case lloutput::Tech::Reflex:
        return LowLatencyMode::Reflex;
    case lloutput::Tech::AntiLag2:
        return LowLatencyMode::AntiLag2;
    case lloutput::Tech::XeLL:
        return LowLatencyMode::XeLL;
    case lloutput::Tech::LatencyFlex:
        return LowLatencyMode::LatencyFlex;
    default:
        return LowLatencyMode::None;
    }
}

lloutput::OutputPlan InputCommon::plan_output()
{
    auto& state = State::Instance();

    lloutput::OutputInputs in;
    in.realNvapi = !fakenvapi::isUsingAsMainNvapi();
    in.xefgOutput = state.activeFgOutput == FGOutput::XeFG;
    in.side = native::lowlatency::XefgSide();
    in.forceLatencyFlex = Config::Instance()->FN_ForceLatencyFlex.value_or_default();
    in.multiFrame = state.dlssgDetectedInterpolationCount > 1 || state.dlssgLastSetMode == sl::DLSSGMode::eDynamic;
    return lloutput::PlanOutput(in);
}

// The tech becomes the output (XeFG's XeLL while XeLL lowers latency), the one before it stops (create_tech_mutex)
void InputCommon::use_output(std::shared_ptr<LowLatencyTech> tech)
{
    auto old_tech = currently_active_tech.exchange(tech);

    if (old_tech != nullptr && old_tech != xefg_xell.load())
    {
        while (old_tech.use_count() > 1)
            std::this_thread::yield();

        old_tech->deinit();
    }

    activeOutput = tech->get_mode();
    output_api = API::DX12;
    output_device = nullptr;

    LOG_INFO("Low latency output: {} (XeFG's)", magic_enum::enum_name(activeOutput.load()));

    tech->set_forced_mode(xefg_paced());
    tech->set_low_latency_override(Config::Instance()->FN_ForceReflex.value_or_default());
    apply_sleep_mode(tech.get());
}

LowLatencyMode InputCommon::default_output(API api)
{
    // TODO: add avaliableOutput, somehow ?
    auto vendorId = IdentifyGpu::getPrimaryGpu().vendorId;

    // Only the device's extensions, there's no XeLL for Vulkan
    if (api == API::Vulkan)
    {
        if (vendorId == VendorId::AMD && VulkanHooks::o_vkAntiLagUpdateAMD != nullptr)
            return LowLatencyMode::AntiLagVk;

#ifdef LOW_LATENCY_INPUTS
        if (vendorId == VendorId::Nvidia && VulkanHooks::o_vkSetLatencySleepModeNV != nullptr)
            return LowLatencyMode::Reflex;
#endif

        return LowLatencyMode::LatencyFlex;
    }

    // XeLL is D3D12 only
    if (vendorId == VendorId::Intel && api != API::DX11)
        return LowLatencyMode::XeLL;

    if (vendorId == VendorId::AMD)
        return LowLatencyMode::AntiLag2;

    if (vendorId == VendorId::Nvidia && !fakenvapi::isUsingAsMainNvapi())
        return LowLatencyMode::Reflex;

    return LowLatencyMode::LatencyFlex;
}

const char* InputCommon::output_unavailable(LowLatencyMode output, API api)
{
    auto vendorId = IdentifyGpu::getPrimaryGpu().vendorId;

    // On D3D the driver's Reflex is reached through nvapi, fakenvapi's is a translation of its own
    if (output == LowLatencyMode::Reflex &&
        (vendorId != VendorId::Nvidia || (api != API::Vulkan && fakenvapi::isUsingAsMainNvapi())))
    {
        return "Needs an Nvidia GPU";
    }

    if ((output == LowLatencyMode::AntiLag2 || output == LowLatencyMode::AntiLagVk) && vendorId != VendorId::AMD)
        return "Needs an AMD GPU";

    return nullptr;
}

LowLatencyMode InputCommon::for_api(LowLatencyMode mode, API api)
{
    if (api == API::Vulkan && mode == LowLatencyMode::AntiLag2)
        return LowLatencyMode::AntiLagVk;

    if (api != API::Vulkan && mode == LowLatencyMode::AntiLagVk)
        return LowLatencyMode::AntiLag2;

    if (api != API::DX12 && mode == LowLatencyMode::XeLL)
        return default_output(api);

    return mode;
}

API InputCommon::device_api(const InputContext& inputContext, IUnknown* pDevice)
{
    if (inputContext.api != API::NotSelected)
        return inputContext.api;

    // Reflex and UE take either D3D device
    thread_local IUnknown* lastDevice = nullptr;
    thread_local API lastApi = API::NotSelected;

    if (pDevice == nullptr)
        return lastApi;

    if (pDevice != lastDevice)
    {
        ID3D11Device* device11 = nullptr;

        if (pDevice->QueryInterface(IID_PPV_ARGS(&device11)) == S_OK)
        {
            device11->Release();
            lastApi = API::DX11;
        }
        else
        {
            lastApi = API::DX12;
        }

        lastDevice = pDevice;
    }

    return lastApi;
}

API InputCommon::register_call(const InputContext& inputContext, IUnknown* pDevice)
{
    auto api = device_api(inputContext, pDevice);

    // Ignore context that Opti creates
    if (!inputContext.localContext)
    {
        if (api != API::NotSelected)
            input_api[static_cast<size_t>(inputContext.caller)] = api;

        available_inputs.fetch_or(1u << static_cast<uint32_t>(inputContext.caller), std::memory_order_relaxed);
    }

    return api;
}

API InputCommon::uses_api()
{
    if (activeInput != LowLatencyInput::None)
    {
        if (auto api = input_api[static_cast<size_t>(activeInput.load())].load(); api != API::NotSelected)
            return api;
    }

    return State::Instance().swapchainApi;
}

std::optional<LowLatencyMode> InputCommon::forced_output()
{
    // The game's XeFG paces through its own XeLL (no output). OptiScaler's XeFG takes XeLL, Reflex or AntiLag 2
    // (xefg_output), its DLSSG and FSR-FG any output, the game's own FSR-FG and DLSSG default to their own
    // (default_input, default_output).
    if (InputXeLL::IsNative())
        return LowLatencyMode::XeLL;

    return std::nullopt;
}

// OptiScaler's XeFG paces with XeLL, the default. The GPU's Reflex or AntiLag 2 output can do the sleeping instead,
// XeFG then keeps an XeLL context of its own (acquire_xefg_xell).
LowLatencyMode InputCommon::xefg_output(LowLatencyMode configured)
{
    if ((configured == LowLatencyMode::Reflex || configured == LowLatencyMode::AntiLag2) &&
        output_unavailable(configured, API::DX12) == nullptr)
    {
        return configured;
    }

    return LowLatencyMode::XeLL;
}

bool InputCommon::xefg_paced()
{
    // OptiScaler's XeFG needs XeLL in low latency mode and knows its generated frames
    return activeOutput == LowLatencyMode::XeLL && State::Instance().activeFgOutput == FGOutput::XeFG;
}

LowLatencyInput InputCommon::default_input()
{
#ifdef LOW_LATENCY_INPUTS
    // With the XeFG input the game's own low latency for its frame generation is XeLL, Reflex markers it also sends
    // belong to its other paths. A game that turns XeLL off uses another frame generation, The Witcher 3 its DLSSG
    // with Reflex.
    if (State::Instance().activeFgInput == FGInput::XeFG && available(LowLatencyInput::XeLL) &&
        get_sleep_copy(LowLatencyInput::XeLL).low_latency_enabled)
    {
        return LowLatencyInput::XeLL;
    }

    // The game's FSR-FG only reports its generated frames through AntiLag 2
    if (available(LowLatencyInput::AntiLag2) && reports_frame_generation(LowLatencyInput::AntiLag2))
        return LowLatencyInput::AntiLag2;
#endif

    // The game's own Reflex, then OptiScaler's own low latency while it runs (Optical F5Low, for frame generation),
    // then the UE Low Latency mod: one sleeper per frame, in the order each stands aside for the one before
    if (available(LowLatencyInput::Reflex))
        return LowLatencyInput::Reflex;

    if (optiscaler_running)
        return LowLatencyInput::OptiScaler;

    if (available(LowLatencyInput::UeLowLatency))
        return LowLatencyInput::UeLowLatency;

#ifdef LOW_LATENCY_INPUTS
    // A game can have XeLL without it being enabled in its settings, it does nothing until it is
    if (available(LowLatencyInput::XeLL) && get_sleep_copy(LowLatencyInput::XeLL).low_latency_enabled)
        return LowLatencyInput::XeLL;

    if (available(LowLatencyInput::AntiLag2))
        return LowLatencyInput::AntiLag2;

    if (available(LowLatencyInput::XeLL))
        return LowLatencyInput::XeLL;
#endif

    return LowLatencyInput::None;
}

const char* InputCommon::incompatibility(LowLatencyInput input, LowLatencyMode output)
{
    // Reflex and XeLL send everything, with frame ids. OptiScaler sends its sleep and present markers, with ids.
    if (input != LowLatencyInput::AntiLag2 && input != LowLatencyInput::UeLowLatency &&
        input != LowLatencyInput::OptiScaler)
    {
        return nullptr;
    }

    if (output == LowLatencyMode::XeLL && input != LowLatencyInput::OptiScaler)
    {
        return input == LowLatencyInput::AntiLag2
                   ? "XeLL needs frame ids and frame markers, AntiLag 2 has neither"
                   : "XeLL needs render submit and present markers, UE only sends simulation markers";
    }

    // The other modes end frames on render submit markers, Reflex ID also sleeps on simulation start markers
    if (output == LowLatencyMode::LatencyFlex &&
        (LFXMode) Config::Instance()->FN_LatencyFlexMode.value_or_default() != LFXMode::Conservative)
    {
        return "LatencyFlex only works in the Conservative mode with this input";
    }

    return nullptr;
}

std::optional<LowLatencyInput> InputCommon::forced_input()
{
    // The game's XeFG (also with the XeFG passthrough) runs on the game's XeLL
#ifdef LOW_LATENCY_INPUTS
    if (XeFGInputs::Passthrough() || InputXeLL::IsNative())
        return LowLatencyInput::XeLL;
#endif

    return std::nullopt;
}

bool InputCommon::update_for_call(const InputContext& inputContext, IUnknown* pDevice)
{
    return update_low_latency_tech(pDevice, register_call(inputContext, pDevice));
}

bool InputCommon::update_low_latency_tech(IUnknown* pDevice, API api, std::optional<LowLatencyMode> mode)
{
    // Nobody calls yet
    if (available_inputs.load(std::memory_order_relaxed) == 0 && !optiscaler_running && !mode.has_value())
        return true;

    // Some outputs might call sleep with nullptr: allowed with an output, not to set one
    if (!pDevice)
        return activeOutput != LowLatencyMode::None && !mode.has_value();

    // Who makes the calls that get through
    if (const auto desiredInput = default_input(); activeInput != desiredInput)
    {
        activeInput = desiredInput;
        LOG_INFO("Low latency input: {}", magic_enum::enum_name(desiredInput));

        // The previous input's frame reports would stay in the overlay, its frame ids don't continue
        clear_reports();

        for (auto& id : last_marker_frame_ids)
            id = 0;

        // The new input's sleep mode
        if (auto current_tech = currently_active_tech.load())
            apply_sleep_mode(current_tech.get());
    }

    // The output runs on the active input's device, another API's device can't start it
    if (auto inputApi = input_api[static_cast<size_t>(activeInput.load())].load();
        !mode.has_value() && inputApi != API::NotSelected && api != inputApi)
    {
        return true;
    }

    // OptiScaler's rules decide the output (OutputRule.h PlanOutput)
    const auto plan = plan_output();
    auto desiredMode = mode.has_value() ? *mode : to_mode(plan.first());

    if (desiredMode == LowLatencyMode::None)
        return true;

    // XeFG paces with its XeLL: while XeLL lowers latency that context is the output, whatever device the call brings
    // (a D3D11 game's through the D3D11-to-D3D12 bridge)
    if (desiredMode == LowLatencyMode::XeLL)
    {
        if (auto own = xefg_xell.load(); own != nullptr)
        {
            if (currently_active_tech.load() != own)
            {
                std::scoped_lock lock(create_tech_mutex);

                if (currently_active_tech.load() != own)
                    use_output(own);
            }

            return true;
        }
    }

    // An output that couldn't start keeps its fallback instead of being retried every call
    const auto failedAt = std::chrono::steady_clock::time_point(std::chrono::steady_clock::duration(failed_time.load()));
    const bool failedRecently =
        desiredMode == failed_output && std::chrono::steady_clock::now() - failedAt < std::chrono::seconds(10);

    if (failedRecently && (currently_active_tech.load() != nullptr || plan.xefgOwnXell))
        return true;

    if (activeOutput == desiredMode && api == output_api)
    {
        delay_deinit = 0;
        return true;
    }

    // Beyond this point activeOutput needs changing

    std::scoped_lock lock(create_tech_mutex);

    if (init_tech(pDevice, api, desiredMode, plan))
        return true;

    auto try_reinit = [&]() -> bool
    {
        if (!deinit_current_tech())
        {
            // Nothing was running: the output could not start
            return currently_active_tech.load() != nullptr;
        }

        activeOutput = LowLatencyMode::None;
        output_api = API::NotSelected;

        return init_tech(pDevice, api, desiredMode, plan);
    };

    // WAR: FSR FG might still be using AntiLag 2, give Opti time to set AL2 context to null
    if (delay_deinit > 0)
    {
        if (--delay_deinit == 0)
            return try_reinit();
    }
    else
    {
        bool al2 = false;
        {
            auto current_tech = currently_active_tech.load();
            if (current_tech && current_tech->get_mode() == LowLatencyMode::AntiLag2)
                al2 = true;
        }

        if (al2)
        {
            delay_deinit = 50;
        }
        else
        {
            return try_reinit();
        }
    }

    return true;
}

void InputCommon::clear_reports()
{
    std::scoped_lock lock(report_mutex);
    std::memset(frame_reports, 0, sizeof(frame_reports));
}

void InputCommon::add_marker_to_report(const MarkerParams& marker_params)
{
    std::scoped_lock lock(report_mutex);

    auto current_timestamp = Util::GetTimestamp() / 1000;
    static auto last_sim_start = current_timestamp;
    static auto _2nd_last_sim_start = current_timestamp;
    auto current_report = &frame_reports[marker_params.frame_id % FRAME_REPORTS_BUFFER_SIZE];

    if (current_report->frameID != marker_params.frame_id)
    {
        *current_report = FrameReport {};
    }

    current_report->frameID = marker_params.frame_id;
    current_report->gpuFrameTimeUs = (uint32_t) (last_sim_start - _2nd_last_sim_start);
    current_report->gpuActiveRenderTimeUs = 100;
    current_report->driverStartTime = current_timestamp;
    current_report->driverEndTime = current_timestamp + 100;
    current_report->gpuRenderStartTime = current_timestamp;
    current_report->gpuRenderEndTime = current_timestamp + 100;
    current_report->osRenderQueueStartTime = current_timestamp;
    current_report->osRenderQueueEndTime = current_timestamp + 100;
    switch (marker_params.marker_type)
    {
    case MarkerType::SIMULATION_START:
        _2nd_last_sim_start = last_sim_start;
        last_sim_start = Util::GetTimestamp() / 1000;
        current_report->simStartTime = last_sim_start;
        break;
    case MarkerType::SIMULATION_END:
        current_report->simEndTime = Util::GetTimestamp() / 1000;
        break;
    case MarkerType::RENDERSUBMIT_START:
        current_report->renderSubmitStartTime = Util::GetTimestamp() / 1000;
        break;
    case MarkerType::RENDERSUBMIT_END:
        current_report->renderSubmitEndTime = Util::GetTimestamp() / 1000;
        break;
    case MarkerType::PRESENT_START:
        current_report->presentStartTime = Util::GetTimestamp() / 1000;
        break;
    case MarkerType::PRESENT_END:
        current_report->presentEndTime = Util::GetTimestamp() / 1000;
        break;
    case MarkerType::INPUT_SAMPLE:
        current_report->inputSampleTime = Util::GetTimestamp() / 1000;
        break;
    default:
        break;
    }
}

// public
void InputCommon::stop_output(bool release)
{
    std::scoped_lock lock(create_tech_mutex);

    auto old_tech = currently_active_tech.exchange(nullptr);

    if (old_tech == nullptr)
        return;

    // XeFG's XeLL is XeFG's to let go of (release_xefg_xell)
    if (old_tech != xefg_xell.load())
    {
        while (old_tech.use_count() > 1)
            std::this_thread::yield();

        if (release)
            old_tech->deinit();
    }

    LOG_INFO("Low latency output {} stopped{}", magic_enum::enum_name(activeOutput.load()),
             release ? "" : " (device gone)");

    clear_reports();
    activeOutput = LowLatencyMode::None;
    output_api = API::NotSelected;
    output_device = nullptr;
}

InputResult InputCommon::set_low_latency_tech(IUnknown* pDevice, LowLatencyMode mode)
{
    if (!update_low_latency_tech(pDevice, API::DX12, mode))
        return InputResult::LowLatencyUpdateFail;

    return InputResult::Ok;
}

InputResult InputCommon::sleep(const InputContext& inputContext, IUnknown* pDevice, std::optional<uint32_t> frame_id)
{
    if (!update_for_call(inputContext, pDevice))
        return InputResult::LowLatencyUpdateFail;

    if (inputContext.caller != activeInput)
        return InputResult::UsingDifferentInput;

    presents_without_input = 0;

    if (auto current_tech = currently_active_tech.load())
        current_tech->sleep(frame_id);
    else
        return InputResult::NoReadyOutput;

    return InputResult::Ok;
}

InputResult InputCommon::set_marker(const InputContext& inputContext, IUnknown* pDevice,
                                    const MarkerParams& marker_params, bool toOutput)
{
    if (!update_for_call(inputContext, pDevice))
        return InputResult::LowLatencyUpdateFail;

    if (inputContext.caller != activeInput)
        return InputResult::UsingDifferentInput;

    if (!marker_supported(inputContext.markerMode, marker_params.marker_type))
        return InputResult::InputNotSupported;

    // Some games send a frame's marker twice (The Witcher 3 ends its render submit again after present), the first
    // one is the right one. Frame id 0 is what inputs without frame ids send.
    if (auto type = (size_t) marker_params.marker_type;
        type < last_marker_frame_ids.size() && marker_params.frame_id != 0)
    {
        if (last_marker_frame_ids[type].exchange(marker_params.frame_id) == marker_params.frame_id)
            return InputResult::Ok;
    }

    presents_without_input = 0;

    if (marker_params.marker_type == MarkerType::PRESENT_START)
        last_present_start_frame_id = marker_params.frame_id;

    // Without frame ids there's nothing to report, the overlay's timings need them
    if (!inputContext.noFrameId)
        add_marker_to_report(marker_params);

    if (!toOutput)
        return InputResult::Ok;

    auto current_tech = currently_active_tech.load();

    // XeFG's own XeLL context follows the game's frames while a Reflex or Anti-Lag 2 output sleeps
    if (marker_params.frame_id != 0)
    {
        if (auto own = xefg_xell.load(); own != nullptr && own != current_tech)
            own->set_marker(pDevice, marker_params);
    }

    // Markers without a frame id only let AntiLag 2 and LatencyFlex follow the sleep calls, Reflex and XeLL use ids
    if (marker_params.frame_id == 0 && (activeOutput == LowLatencyMode::Reflex || activeOutput == LowLatencyMode::XeLL))
    {
        return InputResult::Ok;
    }

    if (current_tech)
        current_tech->set_marker(pDevice, marker_params);
    else
        return InputResult::NoReadyOutput;

    LOG_TRACE_LOWLATENCY("{}: {}", magic_enum::enum_name(marker_params.marker_type), marker_params.frame_id);

    return InputResult::Ok;
}

InputResult InputCommon::set_async_marker(const InputContext& inputContext, ID3D12CommandQueue* pCommandQueue,
                                          const MarkerParams& marker_params)
{
    // Async markers only come with frame generation, the game's own when it's the game's input
    if (!inputContext.localContext)
        mark_frame_generation(inputContext.caller);

    // Always allow Opti's local contexts through
    if (inputContext.caller != activeInput && !inputContext.localContext)
        return InputResult::UsingDifferentInput;

    auto current_tech = currently_active_tech.load();

    if (current_tech == nullptr) // can't init using ID3D12CommandQueue, can only check if available
        return InputResult::LowLatencyUpdateFail;

    if (!marker_supported(inputContext.markerMode, marker_params.marker_type))
        return InputResult::InputNotSupported;

    // With XeFG its XeLL gets them too, as the output when XeLL lowers latency (the game's DLSSG markers while XeFG
    // follows the game's DLSS FG)
    if (auto own = xefg_xell.load(); own != nullptr && own != current_tech)
        own->set_async_marker(pCommandQueue, marker_params);

    current_tech->set_async_marker(pCommandQueue, marker_params);

    LOG_TRACE_LOWLATENCY("{}: {}", magic_enum::enum_name(marker_params.marker_type), marker_params.frame_id);

    return InputResult::Ok;
}

InputResult InputCommon::set_sleep_mode(const InputContext& inputContext, IUnknown* pDevice, SleepMode* sleep_mode)
{
    // Kept even without an output yet, and for an input that is not the active one: AntiLag 2 and XeLL only send it
    // when it changes, and an input that becomes the active one brings its mode along
    set_sleep_copy(inputContext.caller, *sleep_mode);
    sleep_modes_set.fetch_or(1u << static_cast<uint32_t>(inputContext.caller), std::memory_order_relaxed);

    if (!update_for_call(inputContext, pDevice))
        return InputResult::LowLatencyUpdateFail;

    if (inputContext.caller != activeInput)
        return InputResult::UsingDifferentInput;

    if (auto current_tech = currently_active_tech.load())
        apply_sleep_mode(current_tech.get());
    else
        return InputResult::NoReadyOutput;

    return InputResult::Ok;
}

InputResult InputCommon::get_sleep_status(const InputContext& inputContext, IUnknown* pDevice,
                                          SleepParams* sleep_params)
{
    if (!update_for_call(inputContext, pDevice))
        return InputResult::LowLatencyUpdateFail;

    // Get functions don't really need to worry about this check
    // if (inputContext.caller != activeInput)
    //     return InputResult::UsingDifferentInput;

    if (auto current_tech = currently_active_tech.load())
        current_tech->get_sleep_status(sleep_params);
    else
        return InputResult::NoReadyOutput;

    return InputResult::Ok;
}

InputResult InputCommon::get_latency(const InputContext& inputContext, IUnknown* pDev, void* latency_params)
{
    // if (inputContext.caller != activeInput)
    //     return InputResult::UsingDifferentInput;

    if (!update_for_call(inputContext, pDev))
        return InputResult::LowLatencyUpdateFail;

    if (!latency_params)
        return InputResult::InvalidParameter;

    // OptiScaler's own Streamline uses Reflex too
    if (inputContext.caller == LowLatencyInput::Reflex || inputContext.caller == LowLatencyInput::OptiScaler)
    {
        if (activeOutput == LowLatencyMode::Reflex)
        {
            if (auto reflex_tech = std::dynamic_pointer_cast<Reflex>(currently_active_tech.load()))
            {
                if (reflex_tech->get_latency((NV_LATENCY_RESULT_PARAMS*) latency_params) == NVAPI_OK)
                    return InputResult::Ok;
            }
        }

        NV_LATENCY_RESULT_PARAMS* reports = (NV_LATENCY_RESULT_PARAMS*) latency_params;

        if (reports->version != NV_LATENCY_RESULT_PARAMS_VER1)
        {
            LOG_ERROR("Unsupported version {}", reports->version);
            return InputResult::InvalidParameter;
        }

        static_assert(sizeof(reports->frameReport) == NVAPI_BUFFER_SIZE * sizeof(FrameReport));

        if (!copy_frame_reports((FrameReport*) reports->frameReport))
            return InputResult::NotEnoughReports;

        return InputResult::Ok;
    }
    else if (inputContext.caller == LowLatencyInput::XeLL)
    {
        xell_frame_report_t* reports = (xell_frame_report_t*) latency_params;
        constexpr size_t reportCount = 64; // 64 reports, if the app allocated less then it's on them

        if (activeOutput == LowLatencyMode::XeLL)
        {
            if (auto current_tech = currently_active_tech.load())
            {
                auto xell_tech = std::static_pointer_cast<XeLL>(current_tech);
                auto result = xell_tech->xellGetFramesReports(reports);

                if (result == XELL_RESULT_SUCCESS)
                    return InputResult::Ok;
            }
        }

        // Hopefully not too slow
        // XeLL doesn't seem to make any guarantees about ordering so no sort needed
        std::scoped_lock lock(report_mutex);

        for (auto i = 0; i < reportCount; i++)
        {
            auto& reportOut = reports[i];
            auto& reportIn = frame_reports[i];

            reportOut.m_frame_id = reportIn.frameID & 0xFFFFFFFF;
            reportOut.m_sim_start_ts = reportIn.simStartTime;
            reportOut.m_sim_end_ts = reportIn.simEndTime;
            reportOut.m_render_submit_start_ts = reportIn.renderSubmitStartTime;
            reportOut.m_render_submit_end_ts = reportIn.renderSubmitEndTime;
            reportOut.m_present_start_ts = reportIn.presentStartTime;
            reportOut.m_present_end_ts = reportIn.presentEndTime;
        }

        return InputResult::Ok;
    }
    else
    {
        return InputResult::InputNotSupported;
    }
}

bool InputCommon::copy_frame_reports(FrameReport* reports)
{
    std::scoped_lock lock(report_mutex);

    // Assume no frame reports collected yet, report all zeros
    if (frame_reports[FRAME_REPORTS_BUFFER_SIZE - 1].frameID == 0)
    {
        std::memset(reports, 0, NVAPI_BUFFER_SIZE * sizeof(FrameReport));
        // spdlog::warn("GetLatency: Not enough data to report");
        return false;
    }

    // Sort frame reports, find the oldest
    size_t minIdx = 0;
    uint64_t minID = frame_reports[0].frameID;
    for (size_t i = 1; i < FRAME_REPORTS_BUFFER_SIZE; i++)
    {
        if (frame_reports[i].frameID < minID)
        {
            minID = frame_reports[i].frameID;
            minIdx = i;
        }
    }

    // Copy starting from older before wrapping around
    size_t firstChunk = std::min<uint64_t>(NVAPI_BUFFER_SIZE, FRAME_REPORTS_BUFFER_SIZE - minIdx);
    std::memcpy(reports, frame_reports + minIdx, firstChunk * sizeof(FrameReport));

    // Copy the rest after wrapping around
    if (firstChunk < NVAPI_BUFFER_SIZE)
        std::memcpy(reports + firstChunk, frame_reports, (NVAPI_BUFFER_SIZE - firstChunk) * sizeof(FrameReport));

    return true;
}

InputResult InputCommon::get_frame_reports(const InputContext& inputContext, IUnknown* pDev, FrameReport* reports)
{
    if (!update_for_call(inputContext, pDev))
        return InputResult::LowLatencyUpdateFail;

    if (reports == nullptr)
        return InputResult::InvalidParameter;

    return copy_frame_reports(reports) ? InputResult::Ok : InputResult::NotEnoughReports;
}

#define UPDATE_TIMING_ENTRY(name, type)                                                                                \
    if (frameReport.name##EndTime >= frameReport.name##StartTime)                                                      \
    {                                                                                                                  \
        double name##Pos = (double) (frameReport.name##StartTime - start) / rangeNs;                                   \
        double name##Length = (double) (frameReport.name##EndTime - frameReport.name##StartTime) / rangeNs;            \
        timingDataOut.type = TimingEntry { .position = name##Pos, .length = name##Length };                            \
    }                                                                                                                  \
    else                                                                                                               \
    {                                                                                                                  \
        timingDataOut.type.reset();                                                                                    \
    }

bool InputCommon::get_timing_data(TimingData& timingDataOut)
{
    InputContext tempContext {};

    if (activeOutput == LowLatencyMode::Reflex)
    {
        // TODO: allocate struct and get everything from reflex
        timingDataOut = {};
        return false;
    }

    // auto processFrameReport = [&](const auto& frameReport) -> bool
    //{
    //     uint64_t start = UINT64_MAX;
    //     uint64_t end = 0;

    //    // Please don't look, just thought it would be least work
    //    auto pTimes = (const uint64_t*) &frameReport.simStartTime;
    //    for (auto i = 0; i < 11; i++)
    //    {
    //        auto& time = pTimes[i];
    //        if (time == 0)
    //            continue;

    //        if (time < start)
    //            start = time;

    //        if (time > end)
    //            end = time;
    //    }

    //    if (end < start)
    //        return false;

    //    double rangeNs = static_cast<double>(end - start);

    //    timingData[TimingType::TimeRange] = TimingEntry { .position = 0, .length = rangeNs };
    //    UPDATE_TIMING_ENTRY(sim, Simulation)
    //    UPDATE_TIMING_ENTRY(renderSubmit, RenderSubmit)
    //    UPDATE_TIMING_ENTRY(present, Present)
    //    UPDATE_TIMING_ENTRY(driver, Driver)
    //    UPDATE_TIMING_ENTRY(osRenderQueue, OsRenderQueue)
    //    UPDATE_TIMING_ENTRY(gpuRender, GpuRender)

    //    if (frameReport.frameID != 0)
    //        State::Instance().reflexFrameId = frameReport.frameID;

    //    return true;
    //};

    // if (_lastSleepDev && o_NvAPI_D3D_GetLatency)
    //{
    //     // Not calling free on this but it's static so hopefully fine
    //     static NV_LATENCY_RESULT_PARAMS* results = new NV_LATENCY_RESULT_PARAMS();
    //     results->version = NV_LATENCY_RESULT_PARAMS_VER;

    //    if (auto result = hkNvAPI_D3D_GetLatency(_lastSleepDev, results); result != NVAPI_OK)
    //    {
    //        LOG_WARN("NvAPI_D3D_GetLatency failed: {}", magic_enum::enum_name(result));
    //        return false;
    //    }

    //    // 64th element has the latest data
    //    return processFrameReport(results->frameReport[63]);
    //}

    // if (_lastVkSleepDev && o_NvAPI_Vulkan_GetLatency)
    //{
    //     // Not calling free on this but it's static so hopefully fine
    //     static NV_VULKAN_LATENCY_RESULT_PARAMS* results = new NV_VULKAN_LATENCY_RESULT_PARAMS();
    //     results->version = NV_VULKAN_LATENCY_RESULT_PARAMS_VER;

    //    if (auto result = hkNvAPI_Vulkan_GetLatency(_lastVkSleepDev, results); result != NVAPI_OK)
    //    {
    //        LOG_WARN("NvAPI_Vulkan_GetLatency failed: {}", magic_enum::enum_name(result));
    //        return false;
    //    }

    //    // 64th element has the latest data
    //    return processFrameReport(results->frameReport[63]);
    //}

    {
        std::scoped_lock lock(report_mutex);

        size_t maxIdx = 0;
        uint64_t maxID = frame_reports[0].frameID;
        for (size_t i = 1; i < FRAME_REPORTS_BUFFER_SIZE; i++)
        {
            if (frame_reports[i].frameID > maxID)
            {
                maxID = frame_reports[i].frameID;
                maxIdx = i;
            }
        }

        auto highestValidId = (NVAPI_BUFFER_SIZE + maxIdx) % FRAME_REPORTS_BUFFER_SIZE;
        auto& frameReport = frame_reports[highestValidId]; // grab last one

        uint64_t start = UINT64_MAX;
        uint64_t end = 0;

        // Please don't look, just thought it would be least work
        auto pTimes = (const uint64_t*) &frameReport.simStartTime;
        for (auto i = 0; i < 11; i++)
        {
            auto& time = pTimes[i];
            if (time == 0)
                continue;

            if (time < start)
                start = time;

            if (time > end)
                end = time;
        }

        // No reports (an input without frame ids, or one that just changed) or just one marker
        if (end <= start)
        {
            timingDataOut = {};
            return false;
        }

        double rangeNs = static_cast<double>(end - start);

        timingDataOut.timeRange = TimingEntry { .position = 0, .length = rangeNs };
        UPDATE_TIMING_ENTRY(sim, simulation)
        UPDATE_TIMING_ENTRY(renderSubmit, renderSubmit)
        UPDATE_TIMING_ENTRY(present, present)
        UPDATE_TIMING_ENTRY(driver, driver)
        UPDATE_TIMING_ENTRY(osRenderQueue, osRenderQueue)
        UPDATE_TIMING_ENTRY(gpuRender, gpuRender)

        return true;
    };

    return false;
}

// As DLSSG: async present markers on the game's queue around the game's present, with the frame multiplier for
// Reflex Sync in between. Every marker uses the game's frame id.
void InputCommon::fg_game_present(ID3D12CommandQueue* gameQueue, bool presented, uint32_t frameMultiplier)
{
    auto reflex_tech = std::dynamic_pointer_cast<Reflex>(currently_active_tech.load());

    if (reflex_tech == nullptr || gameQueue == nullptr)
        return;

    uint64_t frame_id = last_present_start_frame_id;
    reflex_tech->set_async_marker(gameQueue,
                                  { frame_id, presented ? MarkerType::PRESENT_END : MarkerType::PRESENT_START });

    if (!presented)
    {
        reflex_tech->set_reflex_sync(frameMultiplier);
        fg_frame_id = frame_id;
        fg_new_batch = true;
    }
}

bool InputCommon::output_reports_fg_multiplier()
{
    auto reflex_tech = std::dynamic_pointer_cast<Reflex>(currently_active_tech.load());
    return reflex_tech != nullptr && reflex_tech->has_reflex_sync();
}

// XeFG presents each batch on its own thread, paced over the game's frame, while the game already submits the next
// one. Each batch keeps the frame id XeFG marked it with and the number of frames XeFG sends for it.
namespace
{
struct XeFGBatch
{
    uint64_t frame_id = 0;
    uint32_t frames = 0; // The multiplier until XeFG reports its own count after the game's present
    uint32_t presented = 0;
};

std::mutex xefg_batch_mutex;
std::deque<XeFGBatch> xefg_batches; // Marked, not presented yet
XeFGBatch xefg_batch;               // Presenting
constexpr size_t xefg_max_batches = 8;

void reset_xefg_batches()
{
    std::scoped_lock lock(xefg_batch_mutex);
    xefg_batches.clear();
    xefg_batch = {};
}
} // namespace

// As DLSSG on its present thread: out of band render submit markers around each batch of generated and real
// frames, out of band present markers around each of their presents, all with the game's frame id
void InputCommon::fg_output_present(ID3D12CommandQueue* presentQueue, bool presented, uint32_t frameMultiplier)
{
    // Only the FG presenter thread calls this
    static uint64_t batchFrameId = 0;
    static uint32_t batchPresents = 0;
    static bool batchOpen = false;

    // XeFG marks its batches itself (xefg_marker), each of its presents carries the frame id of its batch. The out of
    // band presents also give AntiLag 2 the frame type, FSR-FG tells AntiLag 2 itself
    // (InputAntiLag2::injectAl2Context).
    if (State::Instance().activeFgOutput == FGOutput::XeFG)
    {
        if (presentQueue == nullptr)
            return;

        if (xefg_present_queue.load() != presentQueue)
        {
            presentQueue->AddRef();

            if (auto old = xefg_present_queue.exchange(presentQueue))
                old->Release();
        }

        auto tech = currently_active_tech.load();

        if (xefg_xell.load() == nullptr || tech == nullptr)
            return;

        uint64_t frame_id = 0;

        {
            std::scoped_lock lock(xefg_batch_mutex);

            // The next batch once this one has all its frames, without a next one it's still this one's
            if (!presented && xefg_batch.presented >= xefg_batch.frames && !xefg_batches.empty())
            {
                xefg_batch = xefg_batches.front();
                xefg_batches.pop_front();
            }
            else if (presented && xefg_batch.frame_id != 0)
            {
                xefg_batch.presented++;
            }

            frame_id = xefg_batch.frame_id;
        }

        if (frame_id != 0)
        {
            tech->set_async_marker(presentQueue, { frame_id, presented ? MarkerType::OUT_OF_BAND_PRESENT_END
                                                                       : MarkerType::OUT_OF_BAND_PRESENT_START });
        }

        return;
    }

    auto reflex_tech = std::dynamic_pointer_cast<Reflex>(currently_active_tech.load());

    if (reflex_tech == nullptr || presentQueue == nullptr)
        return;

    auto send = [&](MarkerType type) { reflex_tech->set_async_marker(presentQueue, { batchFrameId, type }); };

    if (!presented && fg_new_batch.exchange(false))
    {
        // A batch cut short still gets its end
        if (batchOpen)
            send(MarkerType::OUT_OF_BAND_RENDERSUBMIT_END);

        batchFrameId = fg_frame_id;
        batchPresents = 0;
        batchOpen = true;
        send(MarkerType::OUT_OF_BAND_RENDERSUBMIT_START);
    }

    if (batchFrameId == 0)
        return;

    send(presented ? MarkerType::OUT_OF_BAND_PRESENT_END : MarkerType::OUT_OF_BAND_PRESENT_START);

    if (presented && batchOpen && ++batchPresents >= frameMultiplier)
    {
        send(MarkerType::OUT_OF_BAND_RENDERSUBMIT_END);
        batchOpen = false;
    }
}

// AntiLag 2 only knows the generated frames from the frame type set right before each present. DLSSG's out of band
// present markers don't give a usable one with more than 1 generated frame, like OptiScaler's XeFG (fg_output_present)
// the presents themselves do: the first of each batch as the real frame.
bool InputCommon::dlssg_types_presents()
{
    auto fgOutput = State::Instance().activeFgOutput;

    return activeOutput == LowLatencyMode::AntiLag2 && fgOutput != FGOutput::FSRFG && fgOutput != FGOutput::XeFG &&
           State::Instance().dlssgDetectedInterpolationCount > 0;
}

void InputCommon::dlssg_present()
{
    // Only the present thread calls this
    static uint32_t presents = 0;

    if (!dlssg_types_presents())
    {
        presents = 0;
        return;
    }

    auto tech = currently_active_tech.load();

    if (tech == nullptr)
        return;

    auto frames = static_cast<uint32_t>(State::Instance().dlssgDetectedInterpolationCount) + 1;
    bool interpolated = presents++ % frames != 0;

    tech->set_async_marker(nullptr, { 0, MarkerType::OUT_OF_BAND_PRESENT_START, interpolated });
}

void InputCommon::xefg_marker(void* xellContext, uint32_t frame_id, uint32_t marker)
{
    static std::atomic_bool seen[2] {};

    MarkerType type;

    if (marker == 80860000)
        type = MarkerType::OUT_OF_BAND_RENDERSUBMIT_START;
    else if (marker == 80860001)
        type = MarkerType::OUT_OF_BAND_RENDERSUBMIT_END;
    else
    {
        LOG_WARN("Unknown XeFG XeLL marker {} for frame id {}", marker, frame_id);
        return;
    }

    auto index = marker - 80860000;

    if (!seen[index].exchange(true))
        LOG_INFO("XeFG sends XeLL marker {} ({}), frame id {}", marker, magic_enum::enum_name(type), frame_id);

    LOG_TRACE_LOWLATENCY("XeFG {}: {}", magic_enum::enum_name(type), frame_id);

    // Only XeFG's own context next to a Reflex or AntiLag 2 output, the XeLL output's context has them already
    auto own = xefg_xell.load();

    if (own == nullptr || own->get_tech_context() != xellContext)
        return;

    // Without generated frames XeFG sends both right away, DLSSG doesn't send them
    auto fg = State::Instance().currentFG;

    if (fg == nullptr || !fg->IsActive() || fg->IsPaused())
    {
        reset_xefg_batches();
        return;
    }

    if (type == MarkerType::OUT_OF_BAND_RENDERSUBMIT_START)
    {
        xefg_frame_id = frame_id;

        std::scoped_lock lock(xefg_batch_mutex);

        // Nothing takes them while XeFG's presents don't reach the wrapped swapchain, only the latest few are kept
        if (xefg_batches.size() >= xefg_max_batches)
            xefg_batches.pop_front();

        xefg_batches.push_back({ frame_id, fg->GetInterpolatedFrameCount() + 1 });
    }

    // Known after XeFG's first present
    auto queue = xefg_present_queue.load();

    if (auto tech = currently_active_tech.load(); tech != nullptr && queue != nullptr)
        tech->set_async_marker(queue, { frame_id, type });
}

// After the game's present: XeFG's own count for the batch it marked during it. The multiplier can be off, for a
// frame it couldn't generate or a changed frame count.
void InputCommon::xefg_present_status(uint64_t frameIdBefore, uint32_t framesPresented)
{
    auto frame_id = xefg_frame_id.load();

    if (frame_id == 0 || frame_id == frameIdBefore || framesPresented == 0)
        return;

    std::scoped_lock lock(xefg_batch_mutex);

    XeFGBatch* batch = xefg_batch.frame_id == frame_id ? &xefg_batch : nullptr;

    for (auto it = xefg_batches.rbegin(); batch == nullptr && it != xefg_batches.rend(); ++it)
    {
        if (it->frame_id == frame_id)
            batch = &*it;
    }

    if (batch != nullptr)
        batch->frames = framesPresented;
}

// The active input's sleep mode on OptiScaler's terms (OutputRule.h SleepModeFor)
SleepMode InputCommon::shaped_sleep_mode(lloutput::Role role)
{
    const auto input = activeInput.load();
    const auto copy = get_sleep_copy(input);

    lloutput::ShapeInputs in;
    in.xefgOutput = State::Instance().activeFgOutput == FGOutput::XeFG;
    in.side = native::lowlatency::XefgSide();
    // OptiScaler's own calls carry their cap (native::lowlatency PlanCap); the UE mod's stays with the limiter
    in.capIntervalUs = input == LowLatencyInput::Reflex ? fps_limit_interval_us.load() : 0;
    in.forceDisabled = Config::Instance()->FN_ForceReflex.value_or_default() == ForceReflex::ForceDisable;
    in.blocked = input == LowLatencyInput::Reflex && native::lowlatency::GameLowLatencyBlocked();
    in.inputSetMode = (sleep_modes_set.load(std::memory_order_relaxed) & (1u << static_cast<uint32_t>(input))) != 0;

    native::xefgreflex::SleepMode rule;
    rule.lowLatency = copy.low_latency_enabled;
    rule.boost = copy.low_latency_boost;
    rule.minimumIntervalUs = copy.minimum_interval_us;
    rule.useMarkersToOptimize = copy.use_markers_to_optimize;

    const auto shaped = lloutput::SleepModeFor(rule, role, in);

    SleepMode out = copy;
    out.low_latency_enabled = shaped.lowLatency;
    out.low_latency_boost = shaped.boost;
    out.minimum_interval_us = shaped.minimumIntervalUs;
    out.use_markers_to_optimize = shaped.useMarkersToOptimize;
    return out;
}

void InputCommon::apply_sleep_mode(LowLatencyTech* tech)
{
    auto sleepMode = shaped_sleep_mode(lloutput::Role::Output);
    tech->set_sleep_mode(&sleepMode);

    apply_xefg_xell_mode(tech);
}

// XeFG's own XeLL next to another output: everything off, it only keeps XeFG running
void InputCommon::apply_xefg_xell_mode(const LowLatencyTech* output)
{
    if (auto own = xefg_xell.load(); own != nullptr && own.get() != output)
    {
        auto off = shaped_sleep_mode(lloutput::Role::XefgXell);
        own->set_sleep_mode(&off);
    }
}

void InputCommon::update(bool fgActive)
{
    auto& state = State::Instance();
    auto config = Config::Instance();
    auto current_tech = currently_active_tech.load();
    ++present_count;

    // Force State can change at any time, XeLL takes a changed state through its sleep mode
    if (current_tech != nullptr)
    {
        bool xefgPaced = xefg_paced();

        static bool lastXefgPaced = false;
        static ForceReflex lastOverride = ForceReflex::InGame;
        auto lowLatencyOverride = config->FN_ForceReflex.value_or_default();
        current_tech->set_low_latency_override(lowLatencyOverride);
        current_tech->set_forced_mode(xefgPaced);

        if (auto own = xefg_xell.load(); own != nullptr && own != current_tech)
            own->set_low_latency_override(lowLatencyOverride);

        bool overrideChanged = std::exchange(lastOverride, lowLatencyOverride) != lowLatencyOverride;

        if (overrideChanged)
            LOG_INFO("Low latency Force State: {}", magic_enum::enum_name(lowLatencyOverride));

        if (std::exchange(lastXefgPaced, xefgPaced) != xefgPaced || overrideChanged)
            apply_sleep_mode(current_tech.get());
    }

    // fakenvapi as the nvapi: with more than one generated DLSS FG frame its methods fall apart (the plan picks
    // LatencyFlex), and it was told Force Disable for them unless the player forced Reflex on. Never real Reflex.
    {
        static uint8_t lastFrames = 0;
        const auto frames = ReflexHooks::dlssgFrameCountToGenerate();

        if (std::exchange(lastFrames, frames) != frames && fakenvapi::isUsingAsMainNvapi() &&
            state.activeFgOutput != FGOutput::XeFG &&
            config->FN_ForceReflex.value_or_default() != ForceReflex::ForceEnable &&
            (state.dlssgLastSetMode == sl::DLSSGMode::eDynamic || frames > 1))
        {
            LOG_INFO("Multi frame generation, fakenvapi's low latency is told Force Disable");
            config->FN_ForceReflex.set_volatile_value(ForceReflex::ForceDisable);
        }
    }

    // Who keeps the frame cap (OutputRule.h CapForGame): the game's Reflex as the input, or the Streamline of
    // OptiScaler's DLSS FG output (its calls go through the hooks). OptiScaler's own calls carry their cap themselves
    // (native::lowlatency PlanCap, OwnsFrameCap), the UE mod's stays with the limiter.
    const auto input = activeInput.load();
    const bool gameInput = input == LowLatencyInput::Reflex;
    const auto streamline = ReflexHooks::streamlineSleepMode();
    const bool streamlineSource = !gameInput && streamline.set;

    lloutput::GameCap cap;

    if (gameInput || streamlineSource)
    {
        const auto game = gameInput ? ReflexHooks::gameSleepMode() : streamline;
        const auto side = native::lowlatency::XefgSide();
        const bool xefgOutput = state.activeFgOutput == FGOutput::XeFG;
        const bool blocked = native::lowlatency::GameLowLatencyBlocked();
        const bool forceDisabled = config->FN_ForceReflex.value_or_default() == ForceReflex::ForceDisable;
        static const bool intelGpu = IdentifyGpu::getPrimaryGpu().vendorId == VendorId::Intel;

        lloutput::GameCapInputs in;
        in.capFps = config->FramerateLimit.value_or_default();
        in.externalFrameGeneration = state.externalFrameGeneration;
        in.markersRecently = ReflexHooks::markersRecently();
        in.gameSetSleepMode = game.set;
        in.gameLowLatencyOn = game.lowLatency;
        in.gameSendingMarkers = gameInput && ReflexHooks::gameIsSendingMarkers();
        in.realNvapi = !fakenvapi::isUsingAsMainNvapi();
        in.xefgOutput = xefgOutput;
        in.side = side;
        in.splitLimiterKeepsCap =
            native::lowlatency::SplitLimiterKeepsCap(side, state.swapchainInteropApi == SwapchainInteropApi::Dx11wDx12);
        in.xellHoldsCap =
            native::xefgreflex::XellHoldsCap(forceDisabled || blocked || side != lloutput::XefgLatency::XeLL, intelGpu,
                                             native::lowlatency::XefgGenerating());
        in.fsrFgOutput = state.activeFgOutput == FGOutput::FSRFG;
        in.fgActive = fgActive;
        in.blocked = blocked;
        in.xefgShownPerRendered = native::lowlatency::XefgShownPerRendered();
        in.dlssgFramesToGenerate = ReflexHooks::dlssgFrameCountToGenerate();
        in.outputIsXell = activeOutput == LowLatencyMode::XeLL;
        cap = lloutput::CapForGame(in);
    }

    state.reflexLimitsFps = cap.outputHoldsCap;
    state.reflexShowWarning = cap.showWarning;
    limits_fps = cap.outputHoldsCap;

    const auto interval = cap.outputHoldsCap ? lloutput::IntervalUs(cap.fps) : 0u;

    // The game's: in its sleep mode through the output (and XeFG's XeLL)
    if (fps_limit_interval_us.exchange(gameInput ? interval : 0u) != (gameInput ? interval : 0u))
    {
        LOG_INFO("Frame cap through {}: {} us", magic_enum::enum_name(activeOutput.load()), gameInput ? interval : 0u);

        if (current_tech != nullptr)
            apply_sleep_mode(current_tech.get());
    }

    // Streamline's: in its own Reflex mode
    static uint32_t lastStreamlineInterval = 0;

    if (std::exchange(lastStreamlineInterval, streamlineSource ? interval : 0u) != (streamlineSource ? interval : 0u))
        ReflexHooks::streamlineCap(streamlineSource ? interval : 0u);
}

InputResult InputCommon::mark_present_start(IUnknown* pDevice)
{
    // TODO: could allow AL2 but need to check the InputMarkerMode of the active AL2 input
    if (activeInput != LowLatencyInput::UeLowLatency)
        return InputResult::InputNotSupported;

    // TODO: this is missing the frame id required by other outputs
    if (activeOutput != LowLatencyMode::AntiLag2)
        return InputResult::GenericError;

    if (auto current_tech = currently_active_tech.load())
    {
        MarkerParams marker_params {};
        marker_params.frame_id = 0; // TODO: will be needed if used with non-AL2
        marker_params.marker_type = MarkerType::PRESENT_START;

        // AL2 doesn't actually use pDevice but whatever
        current_tech->set_marker(pDevice, marker_params);
    }
    else
    {
        return InputResult::NoReadyOutput;
    }

    return InputResult::Ok;
}

std::shared_ptr<ReflexVk> InputCommon::reflex_vk_output()
{
#ifdef LOW_LATENCY_INPUTS
    return std::dynamic_pointer_cast<ReflexVk>(currently_active_tech.load());
#else
    return nullptr;
#endif
}

xell_context_handle_t InputCommon::xell_output_context()
{
    auto current_tech = currently_active_tech.load();

    if (current_tech == nullptr || current_tech->get_mode() != LowLatencyMode::XeLL)
        return nullptr;

    return (xell_context_handle_t) current_tech->get_tech_context();
}

xell_context_handle_t InputCommon::acquire_xefg_xell(IUnknown* pDevice)
{
    std::scoped_lock lock(create_tech_mutex);

    auto own = xefg_xell.load();

    if (own == nullptr)
    {
        // A XeLL output that runs already (started before XeFG) becomes XeFG's: one XeLL context of ours, the hooked
        // libxell knows one (XellHooks)
        if (auto current = currently_active_tech.load();
            current != nullptr && current->get_mode() == LowLatencyMode::XeLL)
        {
            own = current;
        }
        else
        {
            own = std::make_shared<XeLL>();

            if (!own->init(pDevice))
            {
                LOG_ERROR("Couldn't create XeFG's XeLL context");
                return nullptr;
            }

            // XeFG needs it in low latency mode from the start
            SleepMode sleepMode {};
            sleepMode.low_latency_enabled = true;
            own->set_sleep_mode(&sleepMode);
        }

        own->set_forced_mode(true);
        own->set_low_latency_override(Config::Instance()->FN_ForceReflex.value_or_default());
        xefg_xell.store(own);
        ++xefg_generation;

        LOG_INFO("XeFG paces with OptiScaler's XeLL context");
    }

    // Its mode now: the output's while XeLL lowers latency, else off next to the output; with no input yet it keeps
    // low latency on
    if (activeInput.load() != LowLatencyInput::None)
    {
        if (auto current = currently_active_tech.load(); current == own)
            apply_sleep_mode(own.get());
        else
            apply_xefg_xell_mode(current.get());
    }

    return (xell_context_handle_t) own->get_tech_context();
}

void InputCommon::release_xefg_xell()
{
    std::scoped_lock lock(create_tech_mutex);

    // The markers may still be on their way to it
    if (auto own = xefg_xell.exchange(nullptr))
    {
        // As the output it stops too; the next call starts the output its rules pick
        if (currently_active_tech.load() == own)
        {
            currently_active_tech.store(nullptr);
            activeOutput = LowLatencyMode::None;
            output_api = API::NotSelected;
            output_device = nullptr;
        }

        while (own.use_count() > 1)
            std::this_thread::yield();

        own->deinit();
        LOG_INFO("XeFG's XeLL context released");
    }

    xefg_frame_id = 0;
    reset_xefg_batches();

    if (auto queue = xefg_present_queue.exchange(nullptr))
        queue->Release();
}

void InputCommon::set_optiscaler_running(bool running)
{
    if (optiscaler_running.exchange(running) != running)
        LOG_DEBUG("OptiScaler's own low latency {} as an input", running ? "runs" : "stopped");
}

void InputCommon::device_released(IUnknown* pDevice)
{
    if (pDevice != nullptr && output_device.load() == pDevice)
        stop_output(false);
}

NvAPI_Status InputCommon::xefg_xell_set_sleep_mode(const native::xefgreflex::SleepMode& mode)
{
    auto own = xefg_xell.load();

    if (own == nullptr)
        return NVAPI_ERROR;

    SleepMode sleepMode {};
    sleepMode.low_latency_enabled = mode.lowLatency;
    sleepMode.low_latency_boost = mode.boost;
    sleepMode.minimum_interval_us = mode.minimumIntervalUs;
    sleepMode.use_markers_to_optimize = mode.useMarkersToOptimize;
    own->set_sleep_mode(&sleepMode);
    return NVAPI_OK;
}

NvAPI_Status InputCommon::xefg_xell_sleep()
{
    auto own = xefg_xell.load();

    if (own == nullptr)
        return NVAPI_ERROR;

    own->sleep();
    return NVAPI_OK;
}

void InputCommon::xefg_xell_marker(ID3D12Device* device, uint64_t frame_id, uint32_t nvapiMarker)
{
    MarkerParams marker_params {};
    marker_params.frame_id = frame_id;
    marker_params.marker_type = (MarkerType) nvapiMarker; // the enums match

    if (frame_id != 0)
        add_marker_to_report(marker_params);

    if (auto own = xefg_xell.load(); own != nullptr && device != nullptr)
        own->set_marker(device, marker_params);
}

NvAPI_Status InputCommon::xefg_xell_latency(NV_LATENCY_RESULT_PARAMS* params)
{
    if (params == nullptr || params->version != NV_LATENCY_RESULT_PARAMS_VER1)
        return NVAPI_INVALID_ARGUMENT;

    static_assert(sizeof(params->frameReport) == NVAPI_BUFFER_SIZE * sizeof(FrameReport));
    copy_frame_reports((FrameReport*) params->frameReport);
    return NVAPI_OK;
}

void* InputCommon::output_context()
{
    auto current_tech = currently_active_tech.load();
    return current_tech != nullptr ? current_tech->get_tech_context() : nullptr;
}

NvAPI_Status InputCommon::output_latency(NV_LATENCY_RESULT_PARAMS* params, bool* estimated)
{
    auto current_tech = currently_active_tech.load();

    if (current_tech == nullptr || params == nullptr)
        return NVAPI_ERROR;

    if (auto reflex_tech = std::dynamic_pointer_cast<Reflex>(current_tech))
    {
        *estimated = false;
        return reflex_tech->get_latency(params);
    }

    *estimated = true;
    return copy_frame_reports((FrameReport*) params->frameReport) ? NVAPI_OK : NVAPI_ERROR;
}

void InputCommon::report_fg_present(bool fgState, bool interpolated)
{
    if (auto current_tech = currently_active_tech.load())
    {
        current_tech->set_effective_fg_state(fgState);

        if (fgState)
            current_tech->set_fg_type(interpolated, 0);
    }
}

bool InputCommon::output_enabled()
{
    auto current_tech = currently_active_tech.load();
    return current_tech != nullptr && current_tech->is_enabled();
}

bool InputCommon::xefg_output_change_pending()
{
    // XeFG keeps the XeLL context it got when it started, switching between the XeLL output's and its own needs a
    // restart
    auto started = xefg_started_output.load();

    return started != LowLatencyMode::None &&
           xefg_output(Config::Instance()->LowLatencyOutput.value_or_default()) != started;
}
