// Host check of native/NativeLowLatencyRule.h (when Optical F5Low's own low latency runs and stands aside) and
// native/XefgReflexRule.h (a game's own Reflex with XeFG). No GPU, no game.
// cl /std:c++20 /EHsc /W4 /Iexternal\vulkan\include tests/nr_native_low_latency_smoke.cpp
#include "../OptiScaler/native/XefgReflexRule.h"
#include "../OptiScaler/native/NativeLowLatencyRule.h"
#include "../OptiScaler/native/NativeLowLatencyVkSync.h"
#include "../OptiScaler/framegen/xefg/XeFGFrameId.h"

#include <cstdio>
#include <cstring>
#include <initializer_list>

#include <vulkan/vulkan_core.h>

using namespace native::lowlatency;

static int fails = 0;
#define CHECK(c)                                                                                                       \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(c))                                                                                                      \
        {                                                                                                              \
            printf("FAIL line %d: %s\n", __LINE__, #c);                                                                \
            ++fails;                                                                                                   \
        }                                                                                                              \
    } while (0)

int main()
{
    Inputs in;
    in.f5lowRunning = true;

    // The default: on with an Optical F5Low mode
    CHECK(Decide(in) == Decision::Run);

    // Auto needs an Optical F5Low mode; On does not
    in.f5lowRunning = false;
    CHECK(Decide(in) == Decision::NoF5Low);
    in.setting = Setting::On;
    CHECK(Decide(in) == Decision::Run);

    // Off beats everything
    in.setting = Setting::Off;
    in.f5lowRunning = true;
    CHECK(Decide(in) == Decision::SettingOff);

    // XeFG alone: Auto runs us to feed its XeLL; Off still wins, and the game's own Reflex (which feeds XeLL itself) or
    // XeLL comes first
    {
        Inputs x;
        x.xefgRunning = true;
        CHECK(Decide(x) == Decision::Run);
        x.setting = Setting::Off;
        CHECK(Decide(x) == Decision::SettingOff);
        x.setting = Setting::Auto;
        x.gameCallsReflex = true;
        CHECK(Decide(x) == Decision::GameRunsReflex);
        x.gameCallsReflex = false;
        x.gameRunsXell = true;
        CHECK(Decide(x) == Decision::GameRunsXell);
        x.gameRunsXell = false;
        x.forceXell = true;
        CHECK(Decide(x) == Decision::ForceXell);
    }

    // FSR FG generating (D3D12, or D3D11 through the bridge), no Optical F5Low mode: Auto runs us; Off, the game's own
    // Reflex and fakenvapi's Force Disable still win
    {
        Inputs f;
        f.fsrFgRunning = true;
        CHECK(Decide(f) == Decision::Run);
        CHECK(RunReason(f) == RunsFor::FsrFg);
        f.setting = Setting::Off;
        CHECK(Decide(f) == Decision::SettingOff);
        f.setting = Setting::Auto;
        f.gameCallsReflex = true;
        CHECK(Decide(f) == Decision::GameRunsReflex);
        f.gameCallsReflex = false;
        f.forceReflexDisabled = true;
        CHECK(Decide(f) == Decision::ForceReflexDisabled);
        f.forceReflexDisabled = false;

        // FSR FG paused or off: Auto stops again
        f.fsrFgRunning = false;
        CHECK(Decide(f) == Decision::NoF5Low);

        // What it runs for: an Optical F5Low mode first, then XeFG, then FSR FG; On with nothing keeps the F5Low name
        Inputs r;
        r.fsrFgRunning = true;
        r.f5lowRunning = true;
        CHECK(RunReason(r) == RunsFor::F5Low);
        r.f5lowRunning = false;
        r.xefgRunning = true;
        CHECK(RunReason(r) == RunsFor::Xefg);
        Inputs on;
        on.setting = Setting::On;
        CHECK(RunReason(on) == RunsFor::F5Low);
    }

    // The game's own Reflex: ours never runs, in either setting
    for (auto setting : { Setting::Auto, Setting::On })
    {
        Inputs g;
        g.setting = setting;
        g.f5lowRunning = true;
        g.gameCallsReflex = true;
        CHECK(Decide(g) == Decision::GameRunsReflex);
    }

    // The game's own XeLL: ours stands aside (a second method would be a second wait); its Reflex still comes first
    {
        Inputs x;
        x.setting = Setting::On;
        x.gameRunsXell = true;
        CHECK(Decide(x) == Decision::GameRunsXell);
        x.gameCallsReflex = true;
        CHECK(Decide(x) == Decision::GameRunsReflex);
    }

    // fakenvapi's settings are respected, never overridden
    {
        Inputs f;
        f.f5lowRunning = true;
        f.forceReflexDisabled = true;
        CHECK(Decide(f) == Decision::ForceReflexDisabled);
        f.forceReflexDisabled = false;
        f.forceXell = true;
        CHECK(Decide(f) == Decision::ForceXell);
        f.forceXell = false;
        f.otherFrameGenerationOwner = true;
        CHECK(Decide(f) == Decision::FrameGenerationOwnsReflex);
        f.otherFrameGenerationOwner = false;
        f.apiAvailable = false;
        CHECK(Decide(f) == Decision::NoApi);
    }

    // The game beats the fakenvapi reasons (the more useful line)
    {
        Inputs f;
        f.f5lowRunning = true;
        f.gameCallsReflex = true;
        f.forceReflexDisabled = true;
        CHECK(Decide(f) == Decision::GameRunsReflex);
    }

    // Every reason has its plain text; Run has none
    for (auto d : { Decision::SettingOff, Decision::NoF5Low, Decision::GameRunsReflex, Decision::GameRunsXell,
                    Decision::ForceReflexDisabled, Decision::ForceXell, Decision::FrameGenerationOwnsReflex,
                    Decision::NoApi })
        CHECK(DecisionText(d)[0] != '\0');

    CHECK(DecisionText(Decision::Run)[0] == '\0');
    CHECK(strcmp(DecisionText(Decision::GameRunsReflex), "Use the game's own Reflex settings") == 0);

    // The status label: On when it runs, Standing aside when something else owns latency, Off otherwise
    CHECK(strcmp(StatusLabel(Decision::Run), "On") == 0);

    for (auto d : { Decision::GameRunsReflex, Decision::GameRunsXell, Decision::ForceReflexDisabled,
                    Decision::ForceXell, Decision::FrameGenerationOwnsReflex })
        CHECK(strcmp(StatusLabel(d), "Standing aside") == 0);

    for (auto d : { Decision::SettingOff, Decision::NoF5Low, Decision::NoApi })
        CHECK(strcmp(StatusLabel(d), "Off") == 0);

    // With frame generation's swap chain, only the game's present to it counts; the wrapped swap chain's presents
    // behind it (real and generated frames) are skipped, and count again once frame generation's presents stop
    CHECK(!SwapChainPresentIgnored(5000.0, 0.0));
    CHECK(SwapChainPresentIgnored(5000.0, 4990.0));
    CHECK(SwapChainPresentIgnored(5000.0, 5000.0));
    CHECK(!SwapChainPresentIgnored(5000.0, 5000.0 - kFrameGenerationPresentHoldMs));
    CHECK(!SwapChainPresentIgnored(9000.0, 4990.0));

    // Which Reflex surface a game's present goes through: decided per call from what the caller says, never remembered
    // from an earlier call (a D3D call after a Vulkan one is D3D; no D3D12 device pointer reaches the Vulkan calls).
    {
        int vkDevice = 0, bridgeDevice12 = 0, d3dDevice = 0;

        // A Vulkan game, not bridged: its VkDevice through the Vulkan surface
        auto t = PresentTarget(true, false, &vkDevice, &bridgeDevice12);
        CHECK(t.api == Api::Vulkan && t.device == &vkDevice);

        // Then a D3D game's present (the Vulkan one before it leaves nothing behind)
        t = PresentTarget(false, false, &d3dDevice, nullptr);
        CHECK(t.api == Api::D3D && t.device == &d3dDevice);

        // A bridged Vulkan game: the bridge's private D3D12 device through the D3D surface; no Vulkan call at all
        t = PresentTarget(true, true, &vkDevice, &bridgeDevice12);
        CHECK(t.api == Api::D3D && t.device == &bridgeDevice12);

        // The bridge's device is gone mid-present: nothing to call (and still not the VkDevice through the D3D calls)
        t = PresentTarget(true, true, &vkDevice, nullptr);
        CHECK(t.device == nullptr);

        // And back to the unbridged Vulkan game
        t = PresentTarget(true, false, &vkDevice, &bridgeDevice12);
        CHECK(t.api == Api::Vulkan && t.device == &vkDevice);
    }

    // A device that cannot do the Vulkan pair: its own reason, after the reasons the player can change and before nothing
    {
        Inputs d;
        d.f5lowRunning = true;
        d.deviceAvailable = false;
        CHECK(Decide(d) == Decision::DeviceUnavailable);
        CHECK(DecisionText(Decision::DeviceUnavailable)[0] != '\0');
        CHECK(strcmp(StatusLabel(Decision::DeviceUnavailable), "Off") == 0);
        d.setting = Setting::Off;
        CHECK(Decide(d) == Decision::SettingOff);
        d.setting = Setting::Auto;
        d.gameCallsReflex = true;
        CHECK(Decide(d) == Decision::GameRunsReflex);
    }

    // The Vulkan Sleep pair (NativeLowLatencyVkSync.h): init once, strictly rising values from 1, a wait on each value
    {
        int initCalls = 0, sleepCalls = 0, waitCalls = 0;
        uint64_t lastSleep = 0, lastWait = 0;
        int semaphoreStorage = 0;
        bool timeline = true;
        int initStatus = 0;
        bool reached = true;
        void* semaphore = &semaphoreStorage;

        VkSleepCalls calls;
        calls.timelineSemaphoresOn = [&] { return timeline; };
        calls.init = [&](void** out)
        {
            ++initCalls;
            *out = semaphore;
            return initStatus;
        };
        calls.sleep = [&](uint64_t value)
        {
            ++sleepCalls;
            lastSleep = value;
            return 0;
        };
        calls.wait = [&](void* sem, uint64_t value, uint64_t timeoutNs)
        {
            ++waitCalls;
            lastWait = value;
            CHECK(sem == &semaphoreStorage);
            CHECK(timeoutNs == kVkSleepWaitNs && timeoutNs > 0 && timeoutNs < 5ull * 1000 * 1000 * 1000);
            return reached;
        };

        VkSleepSync sync;
        bool first = false;

        for (uint64_t frame = 1; frame <= 5; ++frame)
        {
            CHECK(VkSleepFrame(sync, calls, &first) == VkSleepResult::Slept);
            CHECK(lastSleep == frame && lastWait == frame);
        }

        CHECK(initCalls == 1 && sleepCalls == 5 && waitCalls == 5);

        // The wait never gets its value: one first-time flag, the value keeps rising
        reached = false;
        CHECK(VkSleepFrame(sync, calls, &first) == VkSleepResult::TimedOut && first && lastSleep == 6);
        CHECK(VkSleepFrame(sync, calls, &first) == VkSleepResult::TimedOut && !first && lastSleep == 7);

        // A wait that comes through again clears the count of timeouts in a row
        reached = true;
        CHECK(VkSleepFrame(sync, calls, &first) == VkSleepResult::Slept && lastSleep == 8);
        reached = false;
        CHECK(VkSleepFrame(sync, calls, &first) == VkSleepResult::TimedOut && !first && lastSleep == 9);
        CHECK(VkSleepFrame(sync, calls, &first) == VkSleepResult::TimedOut && lastSleep == 10);

        // Many timeouts in a row after the driver has signalled before: logged once, tried again, never given up
        CHECK(VkSleepFrame(sync, calls, &first) == VkSleepResult::TimedOut && lastSleep == 11);
        CHECK(VkSleepFrame(sync, calls, &first) == VkSleepResult::TimedOut && lastSleep == 12);
        CHECK(VkSleepFrame(sync, calls, &first) == VkSleepResult::TimedOut && !first && lastSleep == 13);
        CHECK(sync.state == VkSleepState::Ready);
        reached = true;
        CHECK(VkSleepFrame(sync, calls, &first) == VkSleepResult::Slept && lastSleep == 14);

        // A device whose semaphore never reaches a value (the driver does not signal): three in a row and it gives up,
        // no more NVAPI calls at all
        reached = false;
        VkSleepSync silent;
        CHECK(VkSleepFrame(silent, calls, &first) == VkSleepResult::TimedOut && first);
        CHECK(VkSleepFrame(silent, calls, &first) == VkSleepResult::TimedOut && !first);
        CHECK(silent.state == VkSleepState::Ready);
        CHECK(VkSleepFrame(silent, calls, &first) == VkSleepResult::TimedOut);
        CHECK(silent.state == VkSleepState::Unavailable && silent.why == VkSleepWhy::WaitTimedOut);
        const int sleepsBefore = sleepCalls;
        CHECK(VkSleepFrame(silent, calls, &first) == VkSleepResult::Unavailable && sleepCalls == sleepsBefore);

        // A device without the timeline feature: no NVAPI call of any kind
        initCalls = sleepCalls = waitCalls = 0;
        timeline = false;
        VkSleepSync noTimeline;
        CHECK(!VkSleepReady(noTimeline, calls));
        CHECK(VkSleepFrame(noTimeline, calls) == VkSleepResult::Unavailable);
        CHECK(noTimeline.why == VkSleepWhy::NoTimelineSemaphores);
        CHECK(initCalls == 0 && sleepCalls == 0 && waitCalls == 0);

        // A failed init: unavailable for good, the status kept for the log, never a Sleep (fakenvapi would signal a null
        // semaphore), never a second init
        timeline = true;
        initStatus = -5;
        VkSleepSync failed;
        CHECK(VkSleepFrame(failed, calls) == VkSleepResult::Unavailable);
        CHECK(failed.why == VkSleepWhy::InitFailed && failed.initStatus == -5);
        initStatus = 0;
        CHECK(VkSleepFrame(failed, calls) == VkSleepResult::Unavailable);
        CHECK(initCalls == 1 && sleepCalls == 0 && waitCalls == 0);

        // An init that returns OK and no semaphore is as bad
        semaphore = nullptr;
        initCalls = 0;
        VkSleepSync nullSemaphore;
        CHECK(VkSleepFrame(nullSemaphore, calls) == VkSleepResult::Unavailable);
        CHECK(nullSemaphore.why == VkSleepWhy::NoSemaphore);
        CHECK(sleepCalls == 0 && waitCalls == 0);

        // A failing Sleep call is not waited for
        semaphore = &semaphoreStorage;
        VkSleepSync sleepFails;
        calls.sleep = [&](uint64_t) { return -1; };
        CHECK(VkSleepFrame(sleepFails, calls) == VkSleepResult::SleepFailed);
        CHECK(waitCalls == 0);
    }

    // The frame cap while we run (PlanCap): one waiter per frame
    {
        CapInputs cap;

        // Not running: not ours, whatever the cap
        cap.capFps = 60.0f;
        CHECK(PlanCap(cap).owner == CapOwner::NotOurs);
        CHECK(PlanCap(cap).minimumIntervalUs == 0);

        // Running with no cap: ours, nothing to limit (the limiter has nothing to do either)
        cap.running = true;
        cap.capFps = 0.0f;
        CHECK(PlanCap(cap).owner == CapOwner::LowLatency);
        CHECK(PlanCap(cap).minimumIntervalUs == 0);

        // Real Reflex, no frame generation: Reflex keeps 60 fps
        cap.capFps = 60.0f;
        CHECK(PlanCap(cap).owner == CapOwner::LowLatency);
        CHECK(PlanCap(cap).minimumIntervalUs == 16667);

        // Real Reflex + FSR FG where the limiter waits before our Sleep: the limiter keeps it
        cap.fsrFgActive = true;
        cap.limiterBeforeSleep = true;
        CHECK(PlanCap(cap).owner == CapOwner::Limiter);
        CHECK(PlanCap(cap).minimumIntervalUs == 0);

        // ... a cap of 0 does not change who would keep it
        cap.capFps = 0.0f;
        CHECK(PlanCap(cap).owner == CapOwner::Limiter);
        cap.capFps = 60.0f;

        // ... on the Vulkan bridge (limiter after our Sleep): Reflex keeps the game's frames, half of the shown 60
        cap.limiterBeforeSleep = false;
        CHECK(PlanCap(cap).owner == CapOwner::LowLatency);
        CHECK(PlanCap(cap).minimumIntervalUs == 33333);

        // fakenvapi with FSR FG: its method keeps the game's frames
        cap.method = Method::Fakenvapi;
        cap.limiterBeforeSleep = true;
        CHECK(PlanCap(cap).owner == CapOwner::LowLatency);
        CHECK(PlanCap(cap).minimumIntervalUs == 33333);

        // fakenvapi without frame generation
        cap.fsrFgActive = false;
        CHECK(PlanCap(cap).minimumIntervalUs == 16667);

        // XeFG's XeLL counts shown frames
        cap.method = Method::XellWithXefg;
        cap.xefgGenerating = true;
        cap.capFps = 120.0f;
        CHECK(PlanCap(cap).owner == CapOwner::LowLatency);
        CHECK(PlanCap(cap).minimumIntervalUs == 8333);

        // XeFG paused or off on NVIDIA/AMD: XeLL does nothing, the limiter keeps the cap and XeLL gets no interval
        cap.xefgGenerating = false;
        cap.intelGpu = false;
        CHECK(XellIdle(cap));
        CHECK(PlanCap(cap).owner == CapOwner::Limiter);
        CHECK(PlanCap(cap).minimumIntervalUs == 0);

        // ... XeFG generating again: XeLL keeps it
        cap.xefgGenerating = true;
        CHECK(!XellIdle(cap));
        CHECK(PlanCap(cap).owner == CapOwner::LowLatency);
        CHECK(PlanCap(cap).minimumIntervalUs == 8333);

        // ... on Intel XeLL works without XeFG generating
        cap.xefgGenerating = false;
        cap.intelGpu = true;
        CHECK(!XellIdle(cap));
        CHECK(PlanCap(cap).owner == CapOwner::LowLatency);

        // ... XeFG on Intel, paused: XeLL keeps the same shown-frame interval
        CHECK(PlanCap(cap).minimumIntervalUs == 8333);

        // ... with no cap: XeLL generating keeps it with no limit
        cap.xefgGenerating = true;
        cap.capFps = 0.0f;
        CHECK(PlanCap(cap).owner == CapOwner::LowLatency);
        CHECK(PlanCap(cap).minimumIntervalUs == 0);
        cap.capFps = 120.0f;
        cap.xefgGenerating = false;

        // Only XeLL depends on XeFG: real Reflex with XeFG off is never idle
        cap.intelGpu = false;
        cap.method = Method::RealReflex;
        CHECK(!XellIdle(cap));

        // ... nor is fakenvapi's Anti-Lag 2 or LatencyFlex
        cap.method = Method::Fakenvapi;
        CHECK(!XellIdle(cap));
        CHECK(PlanCap(cap).owner == CapOwner::LowLatency);

        // fakenvapi fell back to XeLL (no Anti-Lag 2) on NVIDIA/AMD: no XeFG with it, so it does nothing
        cap.fakenvapiXell = true;
        CHECK(XellIdle(cap));
        CHECK(PlanCap(cap).owner == CapOwner::Limiter);
        CHECK(PlanCap(cap).minimumIntervalUs == 0);

        // ... on Intel it works
        cap.intelGpu = true;
        CHECK(!XellIdle(cap));
        CHECK(PlanCap(cap).owner == CapOwner::LowLatency);
        CHECK(PlanCap(cap).minimumIntervalUs == 8333);
        cap.intelGpu = false;
        cap.fakenvapiXell = false;

        // A refused SetSleepMode: the limiter keeps the cap, whatever the method
        for (auto method : { Method::RealReflex, Method::Fakenvapi, Method::XellWithXefg })
        {
            CapInputs r;
            r.running = true;
            r.capFps = 60.0f;
            r.method = method;
            r.xefgGenerating = true;
            CHECK(PlanCap(r).owner == CapOwner::LowLatency);
            CHECK(OwnerAfterCall(PlanCap(r), true) == CapOwner::Limiter);
            CHECK(OwnerAfterCall(PlanCap(r), false) == CapOwner::LowLatency);
        }

        // ... not running stays not ours
        CHECK(OwnerAfterCall(CapPlan {}, true) == CapOwner::NotOurs);

        // A new plan holds for a few presents before it is taken (frame generation's short pauses)
        {
            const CapPlan xell { CapOwner::LowLatency, 8333 };
            const CapPlan limiter { CapOwner::Limiter, 0 };
            CapSettle settle;

            // The first plan at once
            CHECK(SettleCap(settle, xell) == xell);

            // Frame generation's own pause (IFGFeature::UpdateTarget, 10 frames) is shorter than the settle window
            static_assert(kCapSettlePresents > 10);

            // A pause shorter than kCapSettlePresents: nothing changes
            for (int i = 0; i < kCapSettlePresents - 1; ++i)
                CHECK(SettleCap(settle, limiter) == xell);
            CHECK(SettleCap(settle, xell) == xell);

            // ... and the count starts over after it
            for (int i = 0; i < kCapSettlePresents - 1; ++i)
                CHECK(SettleCap(settle, limiter) == xell);

            // A pause that lasts: taken on its kCapSettlePresents-th present
            CHECK(SettleCap(settle, limiter) == limiter);
            CHECK(SettleCap(settle, limiter) == limiter);

            // A plan that keeps changing never settles on any of its passing values
            const CapPlan other { CapOwner::LowLatency, 16667 };
            for (int i = 0; i < 20; ++i)
                CHECK(SettleCap(settle, i % 2 ? xell : other) == limiter);

            // needed 1: at once
            CHECK(SettleCap(settle, other, 1) == other);
        }

        CHECK(IntervalUs(0.0f) == 0);
        CHECK(IntervalUs(-5.0f) == 0);
        // A tiny ini cap does not overflow
        CHECK(IntervalUs(0.0000001f) == 100000000u);
        CHECK(IntervalUs(0.01f) == 100000000u);
    }

    // When our SetSleepMode goes out again (NeedsSleepModeResend)
    {
        int device = 0;
        int otherDevice = 0;

        SleepModeCall last;
        last.device = &device;
        last.minimumIntervalUs = 16667;
        last.xellGeneration = 3;

        // The first one always goes out
        CHECK(NeedsSleepModeResend(nullptr, false, last, false));

        // Nothing changed: not again, retry due or not
        SleepModeCall now = last;
        CHECK(!NeedsSleepModeResend(&last, false, now, false));
        CHECK(!NeedsSleepModeResend(&last, false, now, true));

        // Each change sends it again
        now.minimumIntervalUs = 8333;
        CHECK(NeedsSleepModeResend(&last, false, now, false));
        now = last;
        now.boost = true;
        CHECK(NeedsSleepModeResend(&last, false, now, false));
        now = last;
        now.device = &otherDevice;
        CHECK(NeedsSleepModeResend(&last, false, now, false));
        now = last;
        now.api = Api::Vulkan;
        CHECK(NeedsSleepModeResend(&last, false, now, false));

        // XeFG made a new XeLL context (same address or not): the generation tells
        now = last;
        now.xellGeneration = 4;
        CHECK(NeedsSleepModeResend(&last, false, now, false));

        // A refused one is tried again only when the retry is due
        now = last;
        CHECK(!NeedsSleepModeResend(&last, true, now, false));
        CHECK(NeedsSleepModeResend(&last, true, now, true));
    }

    // One frame ID for XeLL and XeFG (framegen/xefg/XeFGFrameId.h)
    {
        using xefg::FrameIds;
        using xefg::kSpareFrameIdBase;

        // No XeLL ID: XeFG numbers on its own
        FrameIds<4> ids;
        CHECK(ids.For(1, 0).id == 1);
        CHECK(ids.For(2, 0).id == 2);
        CHECK(!ids.For(3, 0).followsXell);

        // XeLL fed from the next frame on: followed, no reset (nothing followed before)
        auto pick = ids.For(4, 18);
        CHECK(pick.id == 18 && pick.followsXell && !pick.steppedBack && pick.picked);
        CHECK(ids.For(5, 19).id == 19);

        // The frame's ID is kept for all of its uses (tags, constants, present), whatever XeLL says by then
        pick = ids.For(5, 20);
        CHECK(pick.id == 19 && !pick.picked);

        // Low latency started after XeFG numbered thousands of frames: followed downwards with no reset
        FrameIds<4> late;
        for (uint64_t f = 1; f <= 4420; ++f)
            late.For(f, 0);
        pick = late.For(4421, 1);
        CHECK(pick.id == 1 && pick.followsXell && !pick.steppedBack);

        // The game restarts its IDs (NBA 2K27: 241 -> 2): followed, with a history reset
        FrameIds<4> restart;
        for (uint32_t id = 230; id <= 241; ++id)
            restart.For(id, id);
        pick = restart.For(242, 2);
        CHECK(pick.id == 2 && pick.followsXell && pick.steppedBack);
        pick = restart.For(243, 3);
        CHECK(pick.id == 3 && pick.followsXell && !pick.steppedBack);

        // XeLL's ID does not advance (a frame started twice, XeLL a frame behind): a spare, and the next new XeLL
        // ID is followed again with no reset (a spare is not a step back)
        FrameIds<4> twice;
        CHECK(twice.For(1, 50).id == 50);
        pick = twice.For(2, 50);
        CHECK(pick.id == kSpareFrameIdBase && !pick.followsXell && !pick.steppedBack);
        pick = twice.For(3, 51);
        CHECK(pick.id == 51 && pick.followsXell && !pick.steppedBack);

        // An XeLL ID that stays the same for a long time is never followed again (no reset every few frames)
        FrameIds<4> stale;
        stale.For(1, 70);
        int resets = 0;
        int follows = 0;
        for (uint64_t f = 2; f < 40; ++f)
        {
            pick = stale.For(f, 70);
            resets += pick.steppedBack ? 1 : 0;
            follows += pick.followsXell ? 1 : 0;
            CHECK(pick.id != 70);
        }
        CHECK(resets == 0 && follows == 0);

        // XeLL a few frames behind XeFG's own numbers: spares until its IDs are no longer held, then followed for good,
        // with no reset
        FrameIds<4> behind;
        for (uint64_t f = 1; f <= 100; ++f)
            behind.For(f, 0);
        uint32_t xell = 97; // three behind
        bool followed = false;
        int spares = 0;
        uint64_t frame = 101;
        for (; frame < 130 && !followed; ++frame, ++xell)
        {
            pick = behind.For(frame, xell);
            followed = pick.followsXell;
            spares += pick.followsXell ? 0 : 1;
            CHECK(!pick.steppedBack);
        }
        CHECK(followed);
        CHECK(spares <= xefg::FrameIdHistory::kSize);
        pick = behind.For(frame, xell);
        CHECK(pick.followsXell && pick.id == xell);

        // The history holds the newest kSize IDs only
        xefg::FrameIdHistory history;
        for (uint32_t id = 1; id <= xefg::FrameIdHistory::kSize + 1; ++id)
            history.Add(id);
        CHECK(!history.Contains(1));
        CHECK(history.Contains(2));
        CHECK(history.Contains(xefg::FrameIdHistory::kSize + 1));
    }

    // A bridged Vulkan game's own Reflex feeding XeFG's XeLL (native/XefgReflexRule.h)
    {
        using namespace native::xefgreflex;

        // XeFG output and the bridge's device; never dxvk without the Vulkan bridge
        CHECK(ShouldForward(true, true, false));
        CHECK(!ShouldForward(false, true, false)); // another output: the driver keeps the game's Reflex
        CHECK(!ShouldForward(true, false, false)); // no bridge device (plain Vulkan, bridge down or being let go of)
        CHECK(!ShouldForward(true, true, true));   // dxvk with no Vulkan bridge: the D3D hooks and dxvk own it
        CHECK(!ShouldForward(true, false, true));

        // VkLatencyMarkerNV against NVAPI's NV_LATENCY_MARKER_TYPE (NVAPI has PC_LATENCY_PING at 8)
        static_assert(VK_LATENCY_MARKER_SIMULATION_START_NV == 0 && VK_LATENCY_MARKER_TRIGGER_FLASH_NV == 7);
        static_assert(VK_LATENCY_MARKER_OUT_OF_BAND_RENDERSUBMIT_START_NV == 8 &&
                      VK_LATENCY_MARKER_OUT_OF_BAND_PRESENT_END_NV == 11);
        uint32_t nv = 99;

        for (uint32_t vk = 0; vk <= 7; ++vk)
        {
            CHECK(NvApiMarkerFromVulkanNV(vk, &nv) && nv == vk);
        }

        CHECK(NvApiMarkerFromVulkanNV(VK_LATENCY_MARKER_OUT_OF_BAND_RENDERSUBMIT_START_NV, &nv) && nv == 9);
        CHECK(NvApiMarkerFromVulkanNV(VK_LATENCY_MARKER_OUT_OF_BAND_RENDERSUBMIT_END_NV, &nv) && nv == 10);
        CHECK(NvApiMarkerFromVulkanNV(VK_LATENCY_MARKER_OUT_OF_BAND_PRESENT_START_NV, &nv) && nv == 11);
        CHECK(NvApiMarkerFromVulkanNV(VK_LATENCY_MARKER_OUT_OF_BAND_PRESENT_END_NV, &nv) && nv == kNvApiLastMarker);
        CHECK(!NvApiMarkerFromVulkanNV(12, &nv));
        CHECK(!NvApiMarkerFromVulkanNV(0x7FFFFFFF, &nv));

        // XeLL takes the six frame markers only
        for (uint32_t marker = 0; marker <= kNvApiLastMarker; ++marker)
        {
            CHECK(XellTakesMarker(marker) == (marker <= 5));
        }

        // The sleep mode: XeLL keeps the game's values (the cap replaces the interval when one is set), the driver's is
        // all off, the interval too (only one of the two may wait)
        SleepMode game;
        game.lowLatency = true;
        game.boost = true;
        game.minimumIntervalUs = 16667;
        game.useMarkersToOptimize = true;

        auto xell = SleepModeForOutput(game, 0, true, false);
        CHECK(xell.lowLatency && xell.boost && xell.minimumIntervalUs == 16667 && xell.useMarkersToOptimize);
        xell = SleepModeForOutput(game, 8333, true, false);
        CHECK(xell.lowLatency && xell.minimumIntervalUs == 8333);
        CHECK(!kDriverSleepMode.lowLatency && !kDriverSleepMode.boost && kDriverSleepMode.minimumIntervalUs == 0);
        CHECK(!kDriverSleepMode.useMarkersToOptimize);

        // The rule the D3D hooks share: XeFG is the output -> low latency on whatever the game says, the cap in place
        // of the game's interval; only fakenvapi's Force Disable turns it off; no XeFG -> the game's mode as it is
        SleepMode gameOff;
        gameOff.minimumIntervalUs = 16667;
        auto out = SleepModeForOutput(gameOff, 0, true, false);
        CHECK(out.lowLatency && out.minimumIntervalUs == 16667);
        out = SleepModeForOutput(gameOff, 13333, true, false);
        CHECK(out.lowLatency && out.minimumIntervalUs == 13333);
        out = SleepModeForOutput(gameOff, 13333, true, true);
        CHECK(!out.lowLatency && out.minimumIntervalUs == 13333);
        out = SleepModeForOutput(gameOff, 0, false, false);
        CHECK(!out.lowLatency && out.minimumIntervalUs == 16667);
        SleepMode gameOn;
        gameOn.lowLatency = gameOn.boost = true;
        out = SleepModeForOutput(gameOn, 0, false, true);
        CHECK(out.lowLatency && out.boost); // Force Disable is fakenvapi's own business without XeFG

        // Who keeps the cap the game's Reflex hands XeLL: XeLL while it can, OptiScaler's limiter otherwise
        CHECK(XellHoldsCap(false, false, true));   // NVIDIA/AMD, XeFG generating
        CHECK(!XellHoldsCap(false, false, false)); // NVIDIA/AMD, XeFG paused or off: XeLL idle
        CHECK(XellHoldsCap(false, true, false));   // Intel: XeLL works on its own
        CHECK(XellHoldsCap(false, true, true));
        CHECK(!XellHoldsCap(true, true, true));    // Force Disable
        CHECK(!XellHoldsCap(true, false, true));   // Force Disable on NVIDIA/AMD while XeFG generates
        CHECK(!XellHoldsCap(true, true, false));

        // Reflex off in the game's menu: XeLL still runs (its sleeps carry the cap) and gets the cap
        SleepMode off;
        xell = SleepModeForOutput(off, 8333, true, false);
        CHECK(xell.lowLatency && xell.minimumIntervalUs == 8333);

        // The menu: the game's Reflex stands us aside either way, and the text says where it goes
        CHECK(std::strcmp(DecisionText(Decision::GameRunsReflex), "Use the game's own Reflex settings") == 0);
        CHECK(std::strcmp(DecisionText(Decision::GameRunsReflex, true), "The game's Reflex feeds XeFG's XeLL") == 0);
        CHECK(std::strcmp(DecisionText(Decision::GameRunsReflex, true, true),
                          "The game's Reflex feeds XeFG's XeLL, low latency off by Force Disable") == 0);
        CHECK(std::strcmp(DecisionText(Decision::GameRunsReflex, false, true), "Use the game's own Reflex settings") ==
              0);
        CHECK(std::strcmp(DecisionText(Decision::SettingOff, true), DecisionText(Decision::SettingOff)) == 0);
        CHECK(std::strcmp(StatusLabel(Decision::GameRunsReflex), "Standing aside") == 0);

        // The decision itself is unchanged: the game calls Reflex, so we do not
        Inputs x;
        x.xefgRunning = true;
        x.gameCallsReflex = true;
        CHECK(Decide(x) == Decision::GameRunsReflex);
    }

    // Framerate page: the Input choice (Auto / Game / OptiScaler / Off) and what each means
    {
        // Mapping to the setting our own low latency uses, and whether the game's Reflex is sent low latency off
        CHECK(RuleForInput(Input::Auto).setting == Setting::Auto && !RuleForInput(Input::Auto).blockGameLowLatency);
        CHECK(RuleForInput(Input::Game).setting == Setting::Off && !RuleForInput(Input::Game).blockGameLowLatency);
        CHECK(RuleForInput(Input::OptiScaler).setting == Setting::On &&
              !RuleForInput(Input::OptiScaler).blockGameLowLatency);
        CHECK(RuleForInput(Input::Off).setting == Setting::Off && RuleForInput(Input::Off).blockGameLowLatency);

        // Off sends the game's Reflex low latency off, unless DLSS frame generation needs Reflex
        CHECK(BlocksGameLowLatency(Input::Off, false));
        CHECK(!BlocksGameLowLatency(Input::Off, true));
        CHECK(!BlocksGameLowLatency(Input::Game, false));
        CHECK(!BlocksGameLowLatency(Input::Auto, false));
        CHECK(!BlocksGameLowLatency(Input::OptiScaler, false));

        // Ini words: the new key, and the old [DlssNr] NativeLowLatency one
        for (auto input : { Input::Auto, Input::Game, Input::OptiScaler, Input::Off })
        {
            Input back = Input::Auto;
            CHECK(InputFromIniText(InputIniText(input), &back) && back == input);
        }

        Input parsed = Input::Auto;
        CHECK(!InputFromIniText("", &parsed));
        CHECK(!InputFromIniText(nullptr, &parsed));
        CHECK(!InputFromIniText("true", &parsed));
        CHECK(!InputFromIniText("banana", &parsed));

        CHECK(InputFromLegacyText("auto", &parsed) && parsed == Input::Auto);
        CHECK(InputFromLegacyText("true", &parsed) && parsed == Input::OptiScaler);
        CHECK(InputFromLegacyText("false", &parsed) && parsed == Input::Game);
        CHECK(!InputFromLegacyText("game", &parsed));
        CHECK(!InputFromLegacyText(nullptr, &parsed));

        // The old key an older DLL reads: Off and Game both write false (neither makes calls of ours)
        CHECK(std::strcmp(LegacySettingText(Input::Auto), "auto") == 0);
        CHECK(std::strcmp(LegacySettingText(Input::OptiScaler), "true") == 0);
        CHECK(std::strcmp(LegacySettingText(Input::Game), "false") == 0);
        CHECK(std::strcmp(LegacySettingText(Input::Off), "false") == 0);

        // New key wins; a bad new key falls back to the old one; neither: Auto
        CHECK(InputFromIniValues("off", "true") == Input::Off);
        CHECK(InputFromIniValues("game", nullptr) == Input::Game);
        CHECK(InputFromIniValues(nullptr, "true") == Input::OptiScaler);
        CHECK(InputFromIniValues(nullptr, "false") == Input::Game);
        CHECK(InputFromIniValues("banana", "false") == Input::Game);
        CHECK(InputFromIniValues(nullptr, nullptr) == Input::Auto);
        CHECK(InputFromIniValues("", "") == Input::Auto);

        // Decide: Game stands us down with its own answer, Off with its own, both before anything else
        Inputs g;
        g.f5lowRunning = true;
        g.setting = RuleForInput(Input::Game).setting;
        g.blockGameLowLatency = RuleForInput(Input::Game).blockGameLowLatency;
        CHECK(Decide(g) == Decision::SettingOff);

        Inputs o;
        o.f5lowRunning = true;
        o.gameCallsReflex = true;
        o.setting = RuleForInput(Input::Off).setting;
        o.blockGameLowLatency = RuleForInput(Input::Off).blockGameLowLatency;
        CHECK(Decide(o) == Decision::LowLatencyOff);

        // Off kept back by DLSS frame generation: no block, the plain switched-off answer
        o.blockGameLowLatency = BlocksGameLowLatency(Input::Off, true);
        CHECK(Decide(o) == Decision::SettingOff);

        CHECK(std::strcmp(StatusLabel(Decision::LowLatencyOff), "Off") == 0);
        CHECK(DecisionText(Decision::LowLatencyOff)[0] != '\0');
        CHECK(std::strcmp(DecisionText(Decision::LowLatencyOff), DecisionText(Decision::SettingOff)) != 0);

        // OptiScaler runs in any game without Reflex; Auto needs something to run for
        Inputs w;
        w.setting = RuleForInput(Input::OptiScaler).setting;
        CHECK(Decide(w) == Decision::Run);
        w.setting = RuleForInput(Input::Auto).setting;
        CHECK(Decide(w) == Decision::NoF5Low);
    }

    // Framerate page: "Active: <input> -> <output>" names who makes the calls, from the same Decision the code uses
    {
        CHECK(std::strcmp(ActiveInputText(Decision::Run, false), "OptiScaler") == 0);
        CHECK(std::strcmp(ActiveInputText(Decision::LowLatencyOff, true), "Off") == 0);
        CHECK(std::strcmp(ActiveInputText(Decision::FrameGenerationOwnsReflex, true), "Frame generation") == 0);

        // The game's Reflex is what runs when it makes calls, whatever the decision says about us
        CHECK(std::strcmp(ActiveInputText(Decision::GameRunsReflex, true), "Game") == 0);
        CHECK(std::strcmp(ActiveInputText(Decision::NoF5Low, true), "Game") == 0);
        CHECK(std::strcmp(ActiveInputText(Decision::SettingOff, true), "Game") == 0);

        // Nobody makes calls
        CHECK(std::strcmp(ActiveInputText(Decision::NoF5Low, false), "None") == 0);
        CHECK(std::strcmp(ActiveInputText(Decision::SettingOff, false), "None") == 0);
    }

    // Framerate page: the Output choice
    {
        OutputInputs real; // NVIDIA, real nvapi, no XeFG
        CHECK(OutputAvailability(Output::Auto, real).available);
        CHECK(OutputAvailability(Output::XeLL, real).available);
        CHECK(!OutputAvailability(Output::LatencyFlex, real).available);
        CHECK(OutputAvailability(Output::LatencyFlex, real).why[0] != '\0');

        OutputInputs fake;
        fake.fakenvapiMain = true;
        CHECK(OutputAvailability(Output::LatencyFlex, fake).available);

        // XeFG always uses XeLL: Auto and LatencyFlex are out, XeLL is the locked answer
        OutputInputs xefg;
        xefg.fakenvapiMain = true;
        xefg.xefgOutput = true;
        CHECK(!OutputAvailability(Output::LatencyFlex, xefg).available);
        CHECK(!OutputAvailability(Output::Auto, xefg).available);
        CHECK(OutputAvailability(Output::XeLL, xefg).available);
        CHECK(OutputLocked(xefg));
        CHECK(!OutputLocked(real));
        CHECK(OutputShown(xefg, false, false) == Output::XeLL);

        // What the combo shows from the two fakenvapi flags
        CHECK(OutputShown(real, false, false) == Output::Auto);
        CHECK(OutputShown(real, true, false) == Output::XeLL);
        CHECK(OutputShown(fake, false, true) == Output::LatencyFlex);
        CHECK(OutputShown(fake, true, true) == Output::XeLL);

        // Boost: only with real Reflex as the output, and only while OptiScaler makes the calls
        CHECK(BoostAvailable(Input::Auto, real, false));
        CHECK(BoostAvailable(Input::OptiScaler, real, false));
        CHECK(!BoostAvailable(Input::Game, real, false));
        CHECK(!BoostAvailable(Input::Off, real, false));
        CHECK(!BoostAvailable(Input::Auto, fake, false));
        CHECK(!BoostAvailable(Input::Auto, xefg, false));
        CHECK(!BoostAvailable(Input::Auto, real, true)); // Force XeLL
    }

    // Off: the game's sleep mode goes out with low latency off, no boost, no interval (OptiScaler's limiter keeps the cap)
    {
        using namespace native::xefgreflex;

        SleepMode game;
        game.lowLatency = true;
        game.boost = true;
        game.minimumIntervalUs = 8333;
        game.useMarkersToOptimize = true;

        for (const bool xefgOutput : { false, true })
        {
            const auto off = SleepModeForOutput(game, 16666, xefgOutput, false, true);
            CHECK(!off.lowLatency && !off.boost && off.minimumIntervalUs == 0);
            CHECK(off.useMarkersToOptimize); // not ours to change
        }

        // Not blocked: as before (cap in, XeFG keeps low latency on)
        const auto plain = SleepModeForOutput(game, 16666, false, false, false);
        CHECK(plain.lowLatency && plain.boost && plain.minimumIntervalUs == 16666);
        const auto xefgOn = SleepModeForOutput(game, 0, true, false, false);
        CHECK(xefgOn.lowLatency && xefgOn.minimumIntervalUs == 8333);

        // Off and Force Disable together: still off
        CHECK(!SleepModeForOutput(game, 0, true, true, true).lowLatency);
    }

    printf(fails == 0 ? "nr_native_low_latency_smoke: PASS\n" : "nr_native_low_latency_smoke: %d FAIL\n", fails);
    return fails == 0 ? 0 : 1;
}
