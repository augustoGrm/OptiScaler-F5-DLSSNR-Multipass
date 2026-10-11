// Host check of native/NativeLowLatencyRule.h: when Optical F5Low's own low latency runs and stands aside. No GPU, no
// game. cl /std:c++20 /EHsc /W4 tests/nr_native_low_latency_smoke.cpp
#include "../OptiScaler/native/NativeLowLatencyRule.h"
#include "../OptiScaler/native/NativeLowLatencyVkSync.h"

#include <cstdio>
#include <cstring>
#include <initializer_list>

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
    CHECK(strcmp(DecisionText(Decision::GameRunsReflex), "Please use the Game's Own Reflex Settings") == 0);

    // The status label: On when it runs, Standing Aside when something else owns latency, Off otherwise
    CHECK(strcmp(StatusLabel(Decision::Run), "On") == 0);

    for (auto d : { Decision::GameRunsReflex, Decision::GameRunsXell, Decision::ForceReflexDisabled,
                    Decision::ForceXell, Decision::FrameGenerationOwnsReflex })
        CHECK(strcmp(StatusLabel(d), "Standing Aside") == 0);

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
            r.sleepModeFailed = true;
            CHECK(PlanCap(r).owner == CapOwner::Limiter);
            CHECK(PlanCap(r).minimumIntervalUs == 0);
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

    // One frame ID for XeLL and XeFG (NextFrameGenerationId)
    {
        FrameIdHistory recent;
        uint32_t spare = kSpareFrameIdBase;

        auto use = [&](const FrameGenerationId& pick) { recent.Add(pick.id); return pick; };

        // No XeLL ID: XeFG counts on by itself
        CHECK(use(NextFrameGenerationId(0, 0, recent, spare)).id == 1);
        CHECK(use(NextFrameGenerationId(0, 1, recent, spare)).id == 2);
        CHECK(!NextFrameGenerationId(0, 2, recent, spare).followsXell);

        // XeLL fed from its next frame on: XeFG follows a rising ID
        auto pick = use(NextFrameGenerationId(18, 2, recent, spare));
        CHECK(pick.id == 18 && pick.followsXell && !pick.steppedBack);
        CHECK(use(NextFrameGenerationId(19, 18, recent, spare)).id == 19);

        // XeLL's IDs below XeFG's (the game's Reflex began after XeFG had numbered frames itself): followed, with a
        // history reset on the step back, and on from there
        FrameIdHistory r2;
        uint32_t spare2 = kSpareFrameIdBase;
        for (uint32_t id = 4400; id <= 4420; ++id)
            r2.Add(id);

        pick = NextFrameGenerationId(4190, 4420, r2, spare2);
        CHECK(pick.id == 4190 && pick.followsXell && pick.steppedBack);
        r2.Add(pick.id);
        pick = NextFrameGenerationId(4191, 4190, r2, spare2);
        CHECK(pick.id == 4191 && pick.followsXell && !pick.steppedBack);

        // An XeLL ID XeFG still holds (a frame started twice): a spare ID, never a held one
        FrameIdHistory r3;
        uint32_t spare3 = kSpareFrameIdBase;
        r3.Add(50);
        pick = NextFrameGenerationId(50, 50, r3, spare3);
        CHECK(pick.id == kSpareFrameIdBase && !pick.followsXell);
        r3.Add(pick.id);

        // ... and XeLL's next ID is followed again (it was never held)
        pick = NextFrameGenerationId(51, kSpareFrameIdBase, r3, spare3);
        CHECK(pick.id == 51 && pick.followsXell && pick.steppedBack);

        // XeLL a few frames behind XeFG's own count: spares until its IDs are no longer held, then followed for good
        FrameIdHistory r4;
        uint32_t spare4 = kSpareFrameIdBase;
        uint32_t last = 0;
        for (uint32_t id = 1; id <= 100; ++id)
        {
            r4.Add(id);
            last = id;
        }

        uint32_t xell = 97; // three behind
        int spares = 0;
        bool followed = false;
        for (int frame = 0; frame < 20 && !followed; ++frame, ++xell)
        {
            pick = NextFrameGenerationId(xell, last, r4, spare4);
            r4.Add(pick.id);
            last = pick.id;
            followed = pick.followsXell;
            spares += pick.followsXell ? 0 : 1;
            CHECK(pick.id != 0);
        }

        CHECK(followed);
        CHECK(spares <= FrameIdHistory::kSize);

        // ... after which it keeps following
        pick = NextFrameGenerationId(xell, last, r4, spare4);
        CHECK(pick.followsXell && pick.id == xell);

        // The history holds the newest kSize IDs only
        FrameIdHistory r5;
        for (uint32_t id = 1; id <= FrameIdHistory::kSize + 1; ++id)
            r5.Add(id);
        CHECK(!r5.Contains(1));
        CHECK(r5.Contains(2));
        CHECK(r5.Contains(FrameIdHistory::kSize + 1));
    }

    printf(fails == 0 ? "nr_native_low_latency_smoke: PASS\n" : "nr_native_low_latency_smoke: %d FAIL\n", fails);
    return fails == 0 ? 0 : 1;
}
