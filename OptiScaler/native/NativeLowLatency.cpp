#include "pch.h"

#include "NativeLowLatency.h"
#include "DepthFinderCore.h"

#include <Config.h>
#include <State.h>

#include <dlssnr/DlssNr_NativeMode.h>
#include <framegen/IFGFeature_Dx12.h>
#include <hooks/LibraryLoad_Hooks.h>
#include <hooks/Reflex_Hooks.h>
#include <hooks/Vulkan_Hooks.h>
#include <hooks/Xell_Hooks.h>
#include <misc/IdentifyGpu.h>
#include <nvapi/NvApiHooks.h>
#include <nvapi/fakenvapi.h>

#include "NativeLowLatencyVkSync.h"

#include <magic_enum.hpp>

#include <atomic>
#include <mutex>
#include <optional>

namespace native::lowlatency
{

namespace
{
enum class Table
{
    Unknown,
    Found,
    Missing
};

std::atomic<bool> g_running = false;    // markers are being sent
std::atomic<bool> g_submitSent = false; // this frame's SIMULATION_END / RENDERSUBMIT_START went out
// `device` is a D3D11/D3D12 device (the game's, or the Vulkan bridge's private D3D12 device) or -- Api::Vulkan -- the
// game's VkDevice reinterpreted as the same pointer-sized token: never dereferenced as a COM interface here, only passed
// back to the Reflex calls of its own API. Every call carries its Api; nothing is remembered between calls but the Api
// each stored device was seen with (for the callers that bring no device: the first submit, the status line).
std::atomic<Api> g_deviceApi = Api::D3D;
std::atomic<IUnknown*> g_device = nullptr;
std::atomic<Api> g_sleepModeApi = Api::D3D;
std::atomic<IUnknown*> g_sleepModeDevice = nullptr;   // the device our Reflex mode was set on
std::atomic<const void*> g_gameQueue = nullptr;       // the queue the game's frame is submitted to (D3D12)
std::atomic<double> g_frameGenerationPresentAt = 0.0; // last PresentSource::FrameGeneration call, ms
std::atomic<Decision> g_decision = Decision::NoF5Low;
std::atomic<CapOwner> g_capOwner = CapOwner::NotOurs; // PlanCap's answer on the last present we ran on
std::atomic<bool> g_xellIdle = false;                 // XellIdle on the last present we ran on
std::atomic<bool> g_forXefgOnly = false;              // Auto runs us for XeFG's XeLL only, no Optical F5Low mode
// XeLL and XeFG share one frame ID (framegen/xefg/XeFGFrameId.h): the ID of our frame in flight while XeFG runs on our
// XeLL (0: not)
std::atomic<uint32_t> g_xellFrameIdForFrameGeneration = 0;

// Only the game's present thread touches these.
Table g_table = Table::Unknown;
uint64_t g_frameId = 0;
bool g_presentStarted = false; // PRESENT_START of the frame in flight went out, PRESENT_END is owed
bool g_sleepModeSent = false;  // we turned Reflex on
// What our last SetSleepMode carried (NeedsSleepModeResend), whether the method refused it, and when it went out
std::optional<SleepModeCall> g_sent;
bool g_sentRefused = false;
double g_sentAt = 0.0;
CapSettle g_capSettle; // the cap plan in force (SettleCap)

// GetStatus
double g_latencyAskedAt = 0.0;
bool g_hasLatency = false;
float g_latencyMs = 0.0f;
bool g_latencyEstimated = false;

// NV_LATENCY_MARKER_TYPE and NV_VULKAN_LATENCY_MARKER_TYPE share the same ordinals for every value used here, but are
// unrelated enum types in the SDK: translated explicitly rather than cast, so a future SDK change cannot silently
// mismatch them.
NV_VULKAN_LATENCY_MARKER_TYPE VulkanMarkerType(NV_LATENCY_MARKER_TYPE type)
{
    switch (type)
    {
    case SIMULATION_START:
        return VULKAN_SIMULATION_START;
    case SIMULATION_END:
        return VULKAN_SIMULATION_END;
    case RENDERSUBMIT_START:
        return VULKAN_RENDERSUBMIT_START;
    case RENDERSUBMIT_END:
        return VULKAN_RENDERSUBMIT_END;
    case PRESENT_START:
        return VULKAN_PRESENT_START;
    case PRESENT_END:
    default:
        return VULKAN_PRESENT_END;
    }
}

void Marker(Api api, IUnknown* device, NV_LATENCY_MARKER_TYPE type)
{
    if (api == Api::Vulkan)
    {
        NV_VULKAN_LATENCY_MARKER_PARAMS params {};
        params.version = NV_VULKAN_LATENCY_MARKER_PARAMS_VER;
        params.frameID = g_frameId;
        params.markerType = VulkanMarkerType(type);
        ReflexHooks::ownSetLatencyMarkerVulkan(reinterpret_cast<HANDLE>(device), &params);
        return;
    }

    NV_LATENCY_MARKER_PARAMS params {};
    params.version = NV_LATENCY_MARKER_PARAMS_VER;
    params.frameID = g_frameId;
    params.markerType = type;
    ReflexHooks::ownSetLatencyMarker(device, &params);
}

// `minimumIntervalUs`: the frame cap when we keep it (PlanCap), else 0 and OptiScaler's own limiter keeps it.
// ReflexHooks never puts its stored cap into our calls.
NvAPI_Status SetSleepMode(Api api, IUnknown* device, bool on, uint32_t minimumIntervalUs = 0, bool boost = false)
{
    if (api == Api::Vulkan)
    {
        NV_VULKAN_SET_SLEEP_MODE_PARAMS params {};
        params.version = NV_VULKAN_SET_SLEEP_MODE_PARAMS_VER;
        params.bLowLatencyMode = on;
        params.bLowLatencyBoost = on && boost;
        params.minimumIntervalUs = on ? minimumIntervalUs : 0;
        return ReflexHooks::ownSetSleepModeVulkan(reinterpret_cast<HANDLE>(device), &params);
    }

    NV_SET_SLEEP_MODE_PARAMS params {};
    params.version = NV_SET_SLEEP_MODE_PARAMS_VER;
    params.bLowLatencyMode = on;
    params.bLowLatencyBoost = on && boost;
    params.minimumIntervalUs = on ? minimumIntervalUs : 0;
    // The markers are approximate (no engine hooks), so the driver must not tune itself to them
    params.bUseMarkersToOptimize = false;
    params.bUseMinQueueTime = false;
    return ReflexHooks::ownSetSleepMode(device, &params);
}

// The Reflex function table, from the nvapi the game would have loaded: Hook loads it like the game's LoadLibrary
// would (the real nvapi64.dll, or OptiScaler's own fakenvapi when there is no NVIDIA card or no real nvapi) and hooks
// its query function, so a game that loads nvapi later finds it already done. NvAPI_Initialize is counted, so ours is
// harmless next to the game's.
bool FindTable()
{
    if (ReflexHooks::isReflexHooked())
        return true;

    if (NvApiHooks::o_NvAPI_QueryInterface == nullptr)
        LibraryLoadHooks::LoadNvApi();

    auto queryInterface = NvApiHooks::o_NvAPI_QueryInterface;

    if (queryInterface == nullptr)
        return false;

    if (auto init = GET_INTERFACE(NvAPI_Initialize, queryInterface))
        init();

    return ReflexHooks::ensureTable(queryInterface);
}

// The Vulkan sleep pair (NativeLowLatencyVkSync.h) of the game's VkDevice. The game's present thread, and the thread that
// destroys the device.
std::mutex g_vkMutex;
VkSleepSync g_vkSync;
VkDevice g_vkSyncDevice = VK_NULL_HANDLE;
bool g_vkUnavailableLogged = false;

VkSleepCalls VulkanSleepCalls(VkDevice device)
{
    VkSleepCalls calls;
    calls.timelineSemaphoresOn = [device] { return VulkanHooks::TimelineSemaphoresOn(device); };
    calls.init = [device](void** semaphore)
    {
        HANDLE handle = nullptr;
        const auto status = ReflexHooks::ownInitLowLatencyDeviceVulkan(reinterpret_cast<HANDLE>(device), &handle);
        *semaphore = handle;
        return static_cast<int>(status);
    };
    calls.sleep = [device](uint64_t value)
    { return static_cast<int>(ReflexHooks::ownSleepVulkan(reinterpret_cast<HANDLE>(device), value)); };
    calls.wait = [device](void* semaphore, uint64_t value, uint64_t timeoutNs)
    { return VulkanHooks::WaitTimelineSemaphore(device, reinterpret_cast<VkSemaphore>(semaphore), value, timeoutNs); };
    return calls;
}

const char* VulkanWhyText(VkSleepWhy why)
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

// The device can do the Vulkan sleep: initialised once, and no Vulkan NVAPI call is made on one that cannot (fakenvapi
// signals a null semaphore in a Sleep that never had an init). Logs why, once per device.
bool VulkanDeviceReady(VkDevice device)
{
    std::lock_guard lock(g_vkMutex);

    if (g_vkSyncDevice != device)
    {
        g_vkSync = {};
        g_vkSyncDevice = device;
        g_vkUnavailableLogged = false;
    }

    const bool ready = VkSleepReady(g_vkSync, VulkanSleepCalls(device));

    if (!ready && !g_vkUnavailableLogged)
    {
        g_vkUnavailableLogged = true;
        LOG_WARN("Optical F5Low low latency: unavailable on this Vulkan device ({}; NvAPI status {})",
                 VulkanWhyText(g_vkSync.why), g_vkSync.initStatus);
    }

    return ready;
}

// One frame's NvAPI_Vulkan_Sleep and the wait for the semaphore value it signals
void VulkanSleep(VkDevice device)
{
    std::lock_guard lock(g_vkMutex);

    if (g_vkSyncDevice != device)
        return;

    bool firstTimeout = false;
    VkSleepFrame(g_vkSync, VulkanSleepCalls(device), &firstTimeout);

    if (firstTimeout)
        LOG_WARN("Optical F5Low low latency: the Vulkan sleep's semaphore did not reach its value within {} ms",
                 kVkSleepWaitNs / 1000000);

    if (g_vkSync.state == VkSleepState::Unavailable && !g_vkUnavailableLogged)
    {
        g_vkUnavailableLogged = true;
        LOG_WARN("Optical F5Low low latency: unavailable on this Vulkan device ({})", VulkanWhyText(g_vkSync.why));
    }
}

bool ForceReflexDisabled()
{
    return Config::Instance()->FN_ForceReflex.value_or_default() == ForceReflex::ForceDisable;
}

Inputs GatherInputs(bool deviceAvailable)
{
    auto config = Config::Instance();
    auto& state = State::Instance();

    Inputs in;
    in.setting = config->DlssNrNativeLowLatency.value_or_default();

    const auto shown = DlssNrNativeMode::FromKeys(
        { config->DlssNrNativeDepthFinder.value_or_default(), config->DlssNrNativeMotion.value_or_default(),
          config->DlssNrNativeInput.value_or_default(), config->DlssNrNativeUpscaler.value_or_default(),
          config->DlssNrNativeFrameGenerationOnly.value_or_default() });
    in.f5lowRunning = shown != DlssNrNativeMode::Shown::Off && !GameUpscalerCalledRecently();
    in.xefgRunning = state.activeFgOutput == FGOutput::XeFG && state.currentFG != nullptr &&
                     state.currentFG->SwapchainContext() != nullptr;

    in.gameCallsReflex = ReflexHooks::gameCalledReflex();
    in.gameRunsXell = XellHooks::canLimit();
    in.forceReflexDisabled = ForceReflexDisabled();
    in.forceXell = config->ForceXeLL.value_or_default() || state.activeFgInput == FGInput::ForceXeLL;
    in.otherFrameGenerationOwner = state.externalFrameGeneration || state.activeFgOutput == FGOutput::DLSSG;
    in.apiAvailable = g_table != Table::Missing;
    in.deviceAvailable = deviceAvailable;
    return in;
}

Decision Evaluate(Api api, IUnknown* device)
{
    const auto inputs = GatherInputs(true);
    auto decision = Decide(inputs);
    // Run for XeFG alone (no Optical F5Low mode): the menu and log say so
    g_forXefgOnly = !inputs.f5lowRunning && inputs.xefgRunning;

    if (decision == Decision::Run && g_table == Table::Unknown)
    {
        g_table = FindTable() ? Table::Found : Table::Missing;

        if (g_table == Table::Missing)
            LOG_WARN("Optical F5Low low latency: no Reflex or fakenvapi interface found, it stays off");

        decision = Decide(GatherInputs(true));
    }

    // Only a Vulkan device can fail to do it, and only once everything else says go
    if (decision == Decision::Run && api == Api::Vulkan && !VulkanDeviceReady(reinterpret_cast<VkDevice>(device)))
        decision = Decide(GatherInputs(false));

    return decision;
}

// Our Reflex mode is turned off again, unless the game has set its own (its call is the later word)
void Stop(Api api, IUnknown* device)
{
    g_running = false;
    g_xellFrameIdForFrameGeneration = 0;
    g_capOwner = CapOwner::NotOurs;
    g_xellIdle = false;
    g_sent.reset();
    g_sentRefused = false;
    g_capSettle = {};

    if (g_presentStarted && device != nullptr && !ReflexHooks::gameCalledReflex())
        Marker(api, device, PRESENT_END);

    g_presentStarted = false;

    if (auto sleepModeDevice = g_sleepModeDevice.exchange(nullptr);
        g_sleepModeSent && sleepModeDevice != nullptr && !ReflexHooks::gameCalledSetSleepMode())
        SetSleepMode(g_sleepModeApi.load(), sleepModeDevice, false);

    g_sleepModeSent = false;
}

// With frame generation's swap chain in place, its presents are the game's and the wrapped swap chain's are frame
// generation's own (real and generated frames, on its thread)
bool Ignored(PresentSource source)
{
    const auto now = Util::MillisecondsNow();

    if (source == PresentSource::FrameGeneration)
    {
        g_frameGenerationPresentAt = now;
        return false;
    }

    return SwapChainPresentIgnored(now, g_frameGenerationPresentAt.load(std::memory_order_relaxed));
}

const void* GameQueue()
{
    auto& state = State::Instance();

    // Frame generation's swap chain can leave State::currentCommandQueue on its own queue; it knows the game's
    if (auto* fg12 = dynamic_cast<IFGFeature_Dx12*>(state.currentFG);
        fg12 != nullptr && fg12->GameCommandQueue() != nullptr)
        return fg12->GameCommandQueue();

    return state.currentCommandQueue;
}

// XeFG's XeLL routing hangs off the D3D entry points (ReflexHooks): a Vulkan game gets it through the bridge's D3D12
// device, but not through the Vulkan ones
Method CurrentMethod(Api api)
{
    if (State::Instance().activeFgOutput == FGOutput::XeFG && api == Api::D3D)
        return Method::XellWithXefg;

    return fakenvapi::isUsingAsMainNvapi() ? Method::Fakenvapi : Method::RealReflex;
}

CapInputs FrameCapInputs(Api api)
{
    auto& state = State::Instance();
    const bool fgGenerating =
        state.currentFG != nullptr && state.currentFG->IsActive() && !state.currentFG->IsPaused();

    CapInputs in;
    in.running = true;
    in.capFps = Config::Instance()->FramerateLimit.value_or_default();
    in.method = CurrentMethod(api);
    in.fsrFgActive = state.activeFgOutput == FGOutput::FSRFG && fgGenerating;
    in.xefgGenerating = XefgGenerating();
    in.fakenvapiXell = in.method == Method::Fakenvapi && fakenvapi::getCurrentMode() == LowLatencyMode::XeLL;

    // Once: getPrimaryGpu copies the GPU list, and the primary GPU does not change while the game runs
    static const bool intelGpu = IdentifyGpu::getPrimaryGpu().vendorId == VendorId::Intel;
    in.intelGpu = intelGpu;
    // FGHooks::FGPresent's limiter waits before our OnPresentEnd there; the Vulkan bridge's D3D12 present (and its
    // limiter) comes after the game's present, our Sleep included. Only read together with fsrFgActive (FGPresent).
    in.limiterBeforeSleep = state.swapchainInteropApi != SwapchainInteropApi::VkwDx12;
    return in;
}

// Run for XeFG alone (no Optical F5Low mode), the log does not name Optical F5Low
const char* LogName()
{
    return g_forXefgOnly.load() ? "Low latency (feeding XeFG's XeLL)" : "Optical F5Low low latency";
}

std::string CapText(float capFps)
{
    return capFps > 0.0f ? std::format("frame cap {:.0f} fps", capFps) : std::string("no frame cap");
}

const char* PathText(Api api)
{
    const bool viaFakenvapi = CurrentMethod(api) != Method::RealReflex;

    if (!viaFakenvapi)
        return "NVIDIA Reflex";

    switch (fakenvapi::getCurrentMode())
    {
    case LowLatencyMode::AntiLag2:
        return "Anti-Lag 2 (through fakenvapi)";
    case LowLatencyMode::LatencyFlex:
        return "LatencyFlex (through fakenvapi)";
    case LowLatencyMode::XeLL:
        return "XeLL (through fakenvapi)";
    case LowLatencyMode::AntiLagVk:
        return "Anti-Lag (through fakenvapi)";
    default:
        return fakenvapi::isUsingAsMainNvapi() ? "fakenvapi (no method active yet)" : "NVIDIA Reflex";
    }
}

void PresentBegin(Api api, IUnknown* device, PresentSource source)
{
    // Ignored first: a FrameGeneration call must be noted even before we run, ahead of frame generation's own presents
    if (Ignored(source) || !g_running.load(std::memory_order_relaxed) || device == nullptr ||
        ReflexHooks::gameCalledReflex())
    {
        return;
    }

    g_deviceApi = api;
    g_device = device;

    // No submit hook fired (D3D12 sees it only where the queue hook is installed): the frame's work is all behind us
    if (!g_submitSent.exchange(true))
    {
        Marker(api, device, SIMULATION_END);
        Marker(api, device, RENDERSUBMIT_START);
    }

    Marker(api, device, RENDERSUBMIT_END);
    Marker(api, device, PRESENT_START);
    g_presentStarted = true;
}

void PresentEnd(Api api, IUnknown* device, PresentSource source)
{
    if (device == nullptr || State::Instance().isShuttingDown || Ignored(source))
        return;

    const auto decision = Evaluate(api, device);
    const auto previousDecision = g_decision.exchange(decision);

    if (decision != Decision::Run)
    {
        const auto reason = [decision]()
        { return DecisionText(decision, ReflexHooks::gameReflexFeedsXell(), ForceReflexDisabled()); };

        // Once whenever it changes (a running one logs its stop below)
        if (decision != previousDecision && !g_running.load(std::memory_order_relaxed))
            LOG_INFO("{}: not running ({})", LogName(), reason());

        if (g_running.load(std::memory_order_relaxed))
        {
            LOG_INFO("{}: stopped ({})", LogName(), reason());

            // XeLL's frame IDs go on from the game's own now, which can be below ours (Intel: they rise)
            if (decision == Decision::GameRunsReflex && g_xellFrameIdForFrameGeneration.load() != 0)
                LOG_WARN("{}: the game's own Reflex now feeds XeFG's XeLL; its frame IDs may restart below ours",
                         LogName());

            Stop(api, device);
        }

        return;
    }

    // A refusal belongs to the device it came from: a new device (or the old one released) starts over
    if (g_sent && (g_sent->device != device || g_sent->api != api || g_sleepModeDevice.load() != device))
    {
        g_sent.reset();
        g_sentRefused = false;
    }

    auto capInputs = FrameCapInputs(api);
    const auto method = capInputs.method;

    // The plan in force: a new one once it has held for a few presents (frame generation's short pauses)
    const auto planned = PlanCap(capInputs);
    const auto cap = SettleCap(g_capSettle, planned);

    SleepModeCall call;
    call.api = api;
    call.device = device;
    call.minimumIntervalUs = cap.minimumIntervalUs;
    // Only NVIDIA's Reflex has Boost; sent to XeLL it would only be a needless sleep mode change
    call.boost = method == Method::RealReflex && Config::Instance()->DlssNrNativeLowLatencyBoost.value_or_default();
    // XeFG makes a new XeLL context (and sets its sleep mode, with no limit) when its swap chain is made again
    call.xellGeneration = method == Method::XellWithXefg ? fakenvapi::forcedModeGeneration() : 0;

    const auto now = Util::MillisecondsNow();

    if (NeedsSleepModeResend(g_sent ? &*g_sent : nullptr, g_sentRefused, call, now - g_sentAt >= kSleepModeRetryMs))
    {
        const bool wasRefused = g_sentRefused;
        const bool changed =
            !g_sent || g_sent->minimumIntervalUs != call.minimumIntervalUs || g_sent->boost != call.boost;
        const bool newXellContext = g_sent && g_sent->xellGeneration != call.xellGeneration;
        const auto result = SetSleepMode(api, device, true, call.minimumIntervalUs, call.boost);

        g_sleepModeSent = true;
        g_sleepModeApi = api;
        g_sleepModeDevice = device;
        g_sent = call;
        g_sentAt = now;
        g_sentRefused = result != NVAPI_OK;

        if (!g_running.load(std::memory_order_relaxed))
            LOG_INFO("{}: started ({})", LogName(), PathText(api));

        // Refused: the latency calls still go out, but the method never took the cap, so the limiter keeps it
        if (g_sentRefused && !wasRefused)
        {
            LOG_WARN("{}: SetSleepMode refused ({}), OptiScaler's limiter keeps the frame cap", LogName(),
                     magic_enum::enum_name(result));
        }
        else if (!g_sentRefused && newXellContext && !changed)
        {
            LOG_INFO("{}: {} sent again, XeFG made a new XeLL context", LogName(), CapText(capInputs.capFps));
        }
        else if (!g_sentRefused && (changed || wasRefused))
        {
            LOG_INFO("{}: {} in {}, Boost {}", LogName(), CapText(capInputs.capFps),
                     cap.owner == CapOwner::Limiter ? "OptiScaler's limiter" : PathText(api),
                     call.boost ? "on" : "off");
        }
    }

    // XeLL doing nothing cannot hold the cap for the settle window either: the limiter takes it at once (XeLL's interval
    // waits for the settled plan, it does nothing meanwhile anyway)
    g_capOwner = OwnerAfterCall(XellIdle(capInputs) ? planned : cap, g_sentRefused);
    g_xellIdle = XellIdle(capInputs);

    if (g_presentStarted)
        Marker(api, device, PRESENT_END);

    g_presentStarted = false;
    g_deviceApi = api;
    g_device = device;
    g_gameQueue = GameQueue();

    if (api == Api::Vulkan)
        VulkanSleep(reinterpret_cast<VkDevice>(device));
    else
        ReflexHooks::ownSleep(device);

    // XeFG takes this ID for the frame it tags next (FrameIdForFrameGeneration)
    ++g_frameId;
    g_xellFrameIdForFrameGeneration = method == Method::XellWithXefg ? (uint32_t) g_frameId : 0;
    g_submitSent = false;
    g_running = true;
    Marker(api, device, SIMULATION_START);
}
} // namespace

void OnPresentBegin(IUnknown* device, PresentSource source)
{
    PresentBegin(Api::D3D, device, source);
}

void OnPresentEnd(IUnknown* device, PresentSource source)
{
    PresentEnd(Api::D3D, device, source);
}

void OnPresentBeginVulkan(VkDevice device)
{
    PresentBegin(Api::Vulkan, reinterpret_cast<IUnknown*>(device), PresentSource::SwapChain);
}

void OnPresentEndVulkan(VkDevice device)
{
    PresentEnd(Api::Vulkan, reinterpret_cast<IUnknown*>(device), PresentSource::SwapChain);
}

void OnFirstSubmit(const void* queue)
{
    if (!g_running.load(std::memory_order_relaxed) || g_submitSent.load(std::memory_order_relaxed))
        return;

    // A D3D12 queue submit or D3D11 draw says nothing about a frame being paced on the Vulkan surface
    if (g_deviceApi.load(std::memory_order_relaxed) != Api::D3D)
        return;

    // OptiScaler's own queues and frame generation's do not start the game's frame
    if (queue != nullptr)
    {
        if (auto gameQueue = g_gameQueue.load(std::memory_order_relaxed); gameQueue != nullptr && queue != gameQueue)
            return;
    }

    if (g_submitSent.exchange(true))
        return;

    auto device = g_device.load();

    if (device == nullptr || ReflexHooks::gameCalledReflex())
        return;

    Marker(Api::D3D, device, SIMULATION_END);
    Marker(Api::D3D, device, RENDERSUBMIT_START);
}

void OnDeviceReleased(IUnknown* device)
{
    if (device == nullptr)
        return;

    auto expected = device;
    g_device.compare_exchange_strong(expected, nullptr);

    // The next present on a new device sets our Reflex mode again (OnPresentEnd)
    expected = device;
    g_sleepModeDevice.compare_exchange_strong(expected, nullptr);

    ReflexHooks::forgetSleepDevice(device);
}

void OnDeviceReleasedVulkan(VkDevice device)
{
    if (device == nullptr)
        return;

    auto* token = reinterpret_cast<IUnknown*>(device);
    auto expected = token;
    g_device.compare_exchange_strong(expected, nullptr);

    expected = token;
    g_sleepModeDevice.compare_exchange_strong(expected, nullptr);

    ReflexHooks::forgetSleepDeviceVulkan(device);

    // The device's sleep state goes with it (a new device is initialised again)
    std::lock_guard lock(g_vkMutex);

    if (g_vkSyncDevice == device)
    {
        g_vkSync = {};
        g_vkSyncDevice = VK_NULL_HANDLE;
        g_vkUnavailableLogged = false;
    }
}

bool OwnsFrameCap()
{
    return g_running.load(std::memory_order_relaxed) &&
           g_capOwner.load(std::memory_order_relaxed) == CapOwner::LowLatency;
}

bool Running()
{
    return g_decision.load(std::memory_order_relaxed) == Decision::Run && g_running.load(std::memory_order_relaxed);
}

uint32_t FrameIdForFrameGeneration()
{
    if (const auto ours = g_xellFrameIdForFrameGeneration.load(); ours != 0 && Running())
        return ours;

    return 0;
}

bool WantsBoostForFrameGeneration()
{
    return g_decision.load(std::memory_order_relaxed) == Decision::FrameGenerationOwnsReflex &&
           Config::Instance()->DlssNrNativeLowLatencyBoost.value_or_default();
}

bool XefgGenerating()
{
    auto& state = State::Instance();
    const bool fgGenerating =
        state.currentFG != nullptr && state.currentFG->IsActive() && !state.currentFG->IsPaused();

    // XeFG passes frames through in exclusive full screen (Intel); realExclusiveFullscreen is set after
    // FGXeFGForceBorderless had its say, so it means the real thing
    return state.activeFgOutput == FGOutput::XeFG && fgGenerating && !state.realExclusiveFullscreen;
}

void WaitForGameQueueIdle()
{
    static std::mutex mutex;
    static HANDLE event = nullptr; // reused; one a timed-out wait may still signal is let go, never closed

    std::scoped_lock lock(mutex);

    auto* queue = static_cast<ID3D12CommandQueue*>(const_cast<void*>(GameQueue()));

    if (queue == nullptr)
        return;

    if (event == nullptr)
        event = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    ID3D12Device* device = nullptr;

    if (event == nullptr || FAILED(queue->GetDevice(IID_PPV_ARGS(&device))))
        return;

    // A fence of its own each time: held across calls it would keep a released device alive. The calls are rare.
    ID3D12Fence* fence = nullptr;

    if (SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))))
    {
        if (SUCCEEDED(queue->Signal(fence, 1)) && fence->GetCompletedValue() < 1 &&
            SUCCEEDED(fence->SetEventOnCompletion(1, event)) && WaitForSingleObject(event, 250) == WAIT_TIMEOUT)
        {
            LOG_WARN("Low latency: the game's queue did not go idle within 250 ms before a sleep mode change");

            // The fence may still signal it: a new event next time
            event = nullptr;
        }

        fence->Release();
    }

    device->Release();
}

Status GetStatus()
{
    Status status;
    status.decision = g_decision.load();
    status.gameReflexFeedsXell = ReflexHooks::gameReflexFeedsXell();
    status.forceReflexDisabled = ForceReflexDisabled();

    if (status.decision != Decision::Run || !g_running.load(std::memory_order_relaxed))
        return status;

    status.capOwner = g_capOwner.load();
    status.forXefgOnly = g_forXefgOnly.load();
    status.xellIdle = g_xellIdle.load();

    const auto api = g_deviceApi.load();
    status.path = PathText(api);

    auto device = g_device.load();

    if (device == nullptr)
        return status;

    if (const auto now = Util::MillisecondsNow(); now - g_latencyAskedAt >= 500.0)
    {
        g_latencyAskedAt = now;
        g_hasLatency = false;

        // fakenvapi's report has the markers' times only; its GPU end is the last marker's plus 0.1 ms
        const bool estimated = CurrentMethod(api) != Method::RealReflex;
        const auto ReadLatency = [](const auto& report, bool estimated)
        {
            const uint64_t end = estimated ? report.presentEndTime : report.gpuRenderEndTime;

            if (report.frameID != 0 && report.simStartTime != 0 && end > report.simStartTime)
            {
                g_latencyMs = (float) (end - report.simStartTime) / 1000.0f;
                g_latencyEstimated = estimated;
                g_hasLatency = true;
            }
        };

        if (api == Api::Vulkan)
        {
            static NV_VULKAN_LATENCY_RESULT_PARAMS results {};
            results.version = NV_VULKAN_LATENCY_RESULT_PARAMS_VER;

            if (ReflexHooks::ownGetLatencyVulkan(reinterpret_cast<HANDLE>(device), &results) == NVAPI_OK)
            {
                // The newest report is the last; times are in microseconds
                const auto& report = results.frameReport[63];

                ReadLatency(report, estimated);
            }
        }
        else
        {
            static NV_LATENCY_RESULT_PARAMS results {};
            results.version = NV_LATENCY_RESULT_PARAMS_VER;

            if (ReflexHooks::ownGetLatency(device, &results) == NVAPI_OK)
            {
                // The newest report is the last; times are in microseconds
                const auto& report = results.frameReport[63];

                ReadLatency(report, estimated);
            }
        }
    }

    status.hasLatency = g_hasLatency;
    status.latencyMs = g_latencyMs;
    status.latencyEstimated = g_latencyEstimated;
    return status;
}

} // namespace native::lowlatency
