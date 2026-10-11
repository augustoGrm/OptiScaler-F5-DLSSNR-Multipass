#pragma once

// When Optical F5Low's own low latency (native/NativeLowLatency.cpp) runs, and what the menu says when it does not. No
// D3D, no ImGui, no Config: a host test can include it.

#include <cstdint>

namespace native::lowlatency
{
// [DlssNr] NativeLowLatency. Auto: with an Optical F5Low mode running, or XeFG (its XeLL needs someone to feed it). On:
// also without either, in any game that makes no Reflex call of its own. Off: never.
enum class Setting : uint32_t
{
    Auto,
    On,
    Off
};

struct Inputs
{
    Setting setting = Setting::Auto;
    bool f5lowRunning = false;              // an Optical F5Low mode is on and not standing aside for a game upscaler
    bool xefgRunning = false;               // XeFG is the frame generation output and has its swap chain context
    bool gameCallsReflex = false;           // the game has made a Reflex call (SetSleepMode, Sleep or a marker)
    bool gameRunsXell = false;              // the game runs XeLL of its own (with low latency on, so it can limit fps)
    bool forceReflexDisabled = false;       // fakenvapi's Force Reflex is Force Disable
    bool forceXell = false;                 // fakenvapi's Force XeLL (it takes the frame generation slot and runs XeLL)
    bool otherFrameGenerationOwner = false; // frame generation that runs Reflex itself (DLSS FG, an external owner)
    bool apiAvailable = true;               // the Reflex function table could be found
    bool deviceAvailable = true;            // the device can do it (a Vulkan device without a working sleep semaphore cannot)
};

enum class Decision
{
    Run,
    SettingOff,
    NoF5Low,
    GameRunsReflex,
    GameRunsXell,
    ForceReflexDisabled,
    ForceXell,
    FrameGenerationOwnsReflex,
    NoApi,
    DeviceUnavailable
};

// Order matters: what the player can change comes first, then what stops us for another reason.
inline Decision Decide(const Inputs& in)
{
    if (in.setting == Setting::Off)
        return Decision::SettingOff;

    // XeFG cannot work without XeLL (Intel), and XeLL lowers nothing unless someone sleeps and marks frames: with no
    // game Reflex to do it, we do
    if (in.setting == Setting::Auto && !in.f5lowRunning && !in.xefgRunning)
        return Decision::NoF5Low;

    if (in.gameCallsReflex)
        return Decision::GameRunsReflex;

    // Its XeLL already lowers latency and keeps the cap (XellHooks::update): a second method next to it is a second wait
    if (in.gameRunsXell)
        return Decision::GameRunsXell;

    if (in.forceReflexDisabled)
        return Decision::ForceReflexDisabled;

    if (in.forceXell)
        return Decision::ForceXell;

    if (in.otherFrameGenerationOwner)
        return Decision::FrameGenerationOwnsReflex;

    if (!in.apiAvailable)
        return Decision::NoApi;

    if (!in.deviceAvailable)
        return Decision::DeviceUnavailable;

    return Decision::Run;
}

// Which Reflex surface a call goes through: the D3D-flavoured NVAPI entry points (NvAPI_D3D_*, also what XeFG's routing
// to XeLL hangs off) or the Vulkan-flavoured ones (NvAPI_Vulkan_*). Each call carries its own: nothing is remembered
// between calls, so a D3D12 device pointer can never reach a Vulkan entry point or the other way round.
enum class Api
{
    D3D,
    Vulkan
};

struct Target
{
    Api api = Api::D3D;
    const void* device = nullptr; // null: nothing to call
};

// A Vulkan game's present while the frame-generation bridge is up is the bridge's D3D12 swapchain's, on the bridge's
// private D3D12 device: its low latency goes through the D3D surface on that device (so XeFG's XeLL routing reaches it,
// as for a D3D11 game's hidden present) and no Vulkan NVAPI call is made. Without the bridge a Vulkan game's own
// VkDevice goes through the Vulkan surface. A D3D game's present is always D3D.
inline Target PresentTarget(bool vulkanGame, bool bridged, const void* gameDevice, const void* bridgeDevice12)
{
    if (!vulkanGame)
        return { Api::D3D, gameDevice };

    if (bridged)
        return { Api::D3D, bridgeDevice12 };

    return { Api::Vulkan, gameDevice };
}

// Plain words for the menu when we do not run. Run has no text here: the path in use names it.
// Where a present was seen. With OptiScaler's frame generation on a D3D12 game, the game presents to frame
// generation's swap chain (FGHooks::FGPresent) and frame generation then presents every shown frame, real and
// generated, through the wrapped swap chain on its own thread: only the first is the game's frame.
enum class PresentSource
{
    SwapChain,      // the wrapped swap chain (no frame generation swap chain), or the D3D11 bridge's game present
    FrameGeneration // FGHooks::FGPresent, the game's present to frame generation
};

// How long a FrameGeneration present keeps SwapChain presents ignored: frame generation's swap chain can go away
// (turned off, swap chain recreated), and then the wrapped swap chain's presents are the game's again.
inline constexpr double kFrameGenerationPresentHoldMs = 1000.0;

// A SwapChain present is skipped while FrameGeneration presents come in. `lastFrameGenerationMs` is 0 when none came.
inline bool SwapChainPresentIgnored(double nowMs, double lastFrameGenerationMs)
{
    return lastFrameGenerationMs > 0.0 && nowMs - lastFrameGenerationMs < kFrameGenerationPresentHoldMs;
}

// The menu's status line: "Status: [<StatusLabel>]: <reason>". On: it runs (the reason is the method in use). Standing
// Aside: something else owns latency here (the game's Reflex, a fakenvapi setting, frame generation). Off: nothing to
// run on, or switched off.
inline const char* StatusLabel(Decision decision)
{
    switch (decision)
    {
    case Decision::Run:
        return "On";
    case Decision::GameRunsReflex:
    case Decision::GameRunsXell:
    case Decision::ForceReflexDisabled:
    case Decision::ForceXell:
    case Decision::FrameGenerationOwnsReflex:
        return "Standing aside";
    default:
        return "Off";
    }
}

// The reason when it does not run. Run has none here: the method in use names it. `gameReflexFeedsXell`: the game's own
// Reflex calls are sent to XeFG's XeLL (native/BridgedReflexRule.h), so its Reflex settings are not the thing to change.
inline const char* DecisionText(Decision decision, bool gameReflexFeedsXell = false)
{
    switch (decision)
    {
    case Decision::SettingOff:
        return "Switched off";
    case Decision::NoF5Low:
        return "No Optical F5Low mode or XeFG is running";
    case Decision::GameRunsReflex:
        return gameReflexFeedsXell ? "The game's Reflex feeds XeFG's XeLL"
                                   : "Use the game's own Reflex settings";
    case Decision::GameRunsXell:
        return "The game runs XeLL itself";
    case Decision::ForceReflexDisabled:
        return "Force Reflex is set to Force Disable in the fakenvapi settings";
    case Decision::ForceXell:
        return "Force XeLL is on in the fakenvapi settings and already runs XeLL";
    case Decision::FrameGenerationOwnsReflex:
        return "The frame generation in use runs Reflex itself";
    case Decision::NoApi:
        return "No Reflex or fakenvapi interface could be found";
    case Decision::DeviceUnavailable:
        return "Unavailable on this device";
    default:
        return "";
    }
}

// What our calls reach: the method decides how a frame cap is counted.
enum class Method
{
    RealReflex,  // the driver's Reflex (NVIDIA, real nvapi)
    Fakenvapi,   // fakenvapi as the game's nvapi: Anti-Lag 2, XeLL or LatencyFlex
    XellWithXefg // XeFG's XeLL (fakenvapi's context, forced by XeFG on every GPU)
};

// Who keeps [Framerate] FramerateLimit while we run. One waiter per frame: the low-latency method itself when it can
// (NVIDIA and Intel both keep the render queue empty best with their own limit; Intel discourages a second limiter
// next to XeLL), OptiScaler's limiter only where the method's own limit costs more.
enum class CapOwner
{
    NotOurs,    // we do not run: the limiter and the game's own Reflex decide, as before
    LowLatency, // our SetSleepMode carries the cap; OptiScaler's limiter stands aside
    Limiter     // OptiScaler's limiter keeps it (our SetSleepMode carries no limit)
};

struct CapInputs
{
    bool running = false;            // our low latency runs (Decision::Run)
    float capFps = 0.0f;             // FramerateLimit, the shown frame rate; 0: no cap
    Method method = Method::RealReflex;
    bool fsrFgActive = false;        // FSR frame generation is generating frames
    bool limiterBeforeSleep = false; // the limiter's wait comes before our Sleep on this present path (FGPresent)
    bool intelGpu = false;           // XeLL works on its own only on Intel GPUs
    bool xefgGenerating = false;     // XeFG is generating frames (not paused, not off, not in exclusive full screen)
    bool fakenvapiXell = false;      // Method::Fakenvapi has picked XeLL (Anti-Lag 2 missing: fakenvapi falls back)
};

// XeLL on an NVIDIA or AMD GPU does nothing unless XeFG generates frames (Intel: "XeLL will not perform latency
// reduction"): neither the latency nor the cap can be left to it then. fakenvapi's own XeLL never has XeFG with it.
inline bool XellIdle(const CapInputs& in)
{
    if (in.intelGpu)
        return false;

    if (in.method == Method::XellWithXefg)
        return !in.xefgGenerating;

    return in.method == Method::Fakenvapi && in.fakenvapiXell;
}

struct CapPlan
{
    CapOwner owner = CapOwner::NotOurs;
    uint32_t minimumIntervalUs = 0; // for our SetSleepMode; 0: no limit there

    bool operator==(const CapPlan&) const = default;
};

// Clamped as misc/FrameLimit does: a tiny ini cap must not overflow the conversion
inline uint32_t IntervalUs(float fps)
{
    if (fps <= 0.0f)
        return 0u;

    const float us = 1000000.0f / fps + 0.5f;
    return us >= 100000000.0f ? 100000000u : (uint32_t) us;
}

inline CapPlan PlanCap(const CapInputs& in)
{
    if (!in.running)
        return {};

    // Real Reflex's limit with FSR frame generation costs performance (upstream's warning in the Framerate menu); the
    // limiter keeps it where its wait comes before our Sleep. Elsewhere (the Vulkan bridge, whose limiter waits inside
    // the D3D12 present after our Sleep) Reflex's limit is still better than waiting after Sleep.
    if (in.method == Method::RealReflex && in.fsrFgActive && in.limiterBeforeSleep)
        return { CapOwner::Limiter, 0 };

    // XeLL that does nothing on this GPU (XeFG paused or off, or fakenvapi's XeLL with no XeFG): the limiter keeps it
    if (XellIdle(in))
        return { CapOwner::Limiter, 0 };

    if (in.capFps <= 0.0f)
        return { CapOwner::LowLatency, 0 };

    // XeLL counts shown frames (rendered + generated) while XeFG generates; with XeFG paused on an Intel GPU shown
    // frames are the rendered ones, so the same interval holds
    if (in.method == Method::XellWithXefg)
        return { CapOwner::LowLatency, IntervalUs(in.capFps) };

    // Reflex, Anti-Lag 2 and LatencyFlex pace the game's own frames: FSR frame generation doubles them
    return { CapOwner::LowLatency, IntervalUs(in.fsrFgActive ? in.capFps / 2.0f : in.capFps) };
}

// Who keeps the cap after our SetSleepMode: a method that refused it never took the limit, so the limiter keeps it
// until a resend goes through
inline CapOwner OwnerAfterCall(const CapPlan& plan, bool refused)
{
    return refused && plan.owner != CapOwner::NotOurs ? CapOwner::Limiter : plan.owner;
}

// A new plan is taken once it has held for `kCapSettlePresents` presents: frame generation pauses for a few frames
// (its toggles, a changed interpolation count; IFGFeature::UpdateTarget pauses 10) would otherwise hand the cap back
// and forth, each time with a sleep mode change (Intel: GPU idle first, R11). The first plan is taken at once. XeLL
// that does nothing (XellIdle) gives the cap to the limiter at once all the same (NativeLowLatency.cpp PresentEnd).
inline constexpr int kCapSettlePresents = 16;

struct CapSettle
{
    bool started = false;
    CapPlan applied;
    CapPlan pending;
    int held = 0; // presents `pending` has held
};

inline CapPlan SettleCap(CapSettle& settle, const CapPlan& planned, int needed = kCapSettlePresents)
{
    if (!settle.started || needed <= 1)
    {
        settle = { true, planned, planned, 0 };
        return planned;
    }

    if (planned == settle.applied)
    {
        settle.pending = planned;
        settle.held = 0;
        return settle.applied;
    }

    if (planned == settle.pending)
        ++settle.held;
    else
        settle.pending = planned, settle.held = 1;

    if (settle.held >= needed)
    {
        settle.applied = planned;
        settle.held = 0;
    }

    return settle.applied;
}

// What one SetSleepMode of ours carried, and whether the method took it
struct SleepModeCall
{
    Api api = Api::D3D;
    const void* device = nullptr;
    uint32_t minimumIntervalUs = 0;
    bool boost = false;
    uint32_t xellGeneration = 0; // fakenvapi::forcedModeGeneration while XeFG's XeLL is the method, else 0
};

inline bool SameSleepMode(const SleepModeCall& a, const SleepModeCall& b)
{
    return a.api == b.api && a.device == b.device && a.minimumIntervalUs == b.minimumIntervalUs &&
           a.boost == b.boost && a.xellGeneration == b.xellGeneration;
}

// Our SetSleepMode goes out once, then again whenever what it carries changes: a new device, the cap, Boost, or a new
// XeLL context from XeFG (made with no limit, XeFG_Dx12). A refused one is tried again, at most once a `retryDue`.
// `last` is null before the first.
inline bool NeedsSleepModeResend(const SleepModeCall* last, bool lastRefused, const SleepModeCall& now, bool retryDue)
{
    if (last == nullptr || !SameSleepMode(*last, now))
        return true;

    return lastRefused && retryDue;
}

// How often a refused SetSleepMode is tried again
inline constexpr double kSleepModeRetryMs = 1000.0;

}// namespace native::lowlatency
