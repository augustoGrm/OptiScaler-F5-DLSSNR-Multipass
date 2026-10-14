// Host check of OutputRule.h: InputCommon's outputs on OptiScaler's terms. No GPU and no game needed.
// cl /std:c++20 /EHsc /W4 tests/ll_output_rule_smoke.cpp
#include "../OptiScaler/low_latency/input/OutputRule.h"

#include <cstdio>

using namespace lloutput;

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

static bool Same(const SleepMode& a, const SleepMode& b)
{
    return a.lowLatency == b.lowLatency && a.boost == b.boost && a.minimumIntervalUs == b.minimumIntervalUs &&
           a.useMarkersToOptimize == b.useMarkersToOptimize;
}

static SleepMode Mode(bool lowLatency, bool boost, uint32_t intervalUs, bool markers = true)
{
    SleepMode m;
    m.lowLatency = lowLatency;
    m.boost = boost;
    m.minimumIntervalUs = intervalUs;
    m.useMarkersToOptimize = markers;
    return m;
}

int main()
{
    // ---- PlanOutput ----
    {
        OutputInputs in;

        // NVIDIA, real nvapi: the driver's Reflex
        in.realNvapi = true;
        auto p = PlanOutput(in);
        CHECK(p.first() == Tech::Reflex && p.tries[1] == Tech::None && !p.xefgOwnXell);

        // fakenvapi as the nvapi: Anti-Lag 2, then XeLL, then LatencyFlex
        in.realNvapi = false;
        p = PlanOutput(in);
        CHECK(p.tries[0] == Tech::AntiLag2 && p.tries[1] == Tech::XeLL && p.tries[2] == Tech::LatencyFlex);

        // Force LatencyFlex, or more than one generated DLSS FG frame: LatencyFlex only
        in.forceLatencyFlex = true;
        CHECK(PlanOutput(in).first() == Tech::LatencyFlex && PlanOutput(in).tries[1] == Tech::None);
        in.forceLatencyFlex = false;
        in.multiFrame = true;
        CHECK(PlanOutput(in).first() == Tech::LatencyFlex);
        in.multiFrame = false;

        // XeFG: XeLL is the output, XeFG paces with its context
        in.xefgOutput = true;
        p = PlanOutput(in);
        CHECK(p.first() == Tech::XeLL && !p.xefgOwnXell);

        // ... also with Force LatencyFlex or multi frame (XeFG decides)
        in.forceLatencyFlex = true;
        CHECK(PlanOutput(in).first() == Tech::XeLL);
        in.forceLatencyFlex = false;

        // XeFG with Reflex or Anti-Lag 2 lowering latency: that output, XeFG gets its own XeLL
        in.side = XefgLatency::Reflex;
        p = PlanOutput(in);
        CHECK(p.first() == Tech::Reflex && p.xefgOwnXell);
        in.side = XefgLatency::AntiLag2;
        p = PlanOutput(in);
        CHECK(p.first() == Tech::AntiLag2 && p.xefgOwnXell);

        // An input that can't feed XeLL (the UE mod, Anti-Lag 2: XL18): never XeLL as the output
        in.inputFeedsXell = false;
        CHECK(PlanOutput(in).first() == Tech::AntiLag2); // the split is not XeLL's anyway
        in.side = XefgLatency::XeLL;
        p = PlanOutput(in);
        CHECK(p.first() == Tech::LatencyFlex && p.tries[1] == Tech::None && p.xefgOwnXell); // XeFG keeps its own
        in.xefgOutput = false;
        p = PlanOutput(in);
        CHECK(p.tries[0] == Tech::AntiLag2 && p.tries[1] == Tech::LatencyFlex && p.tries[2] == Tech::None);
        in.realNvapi = true;
        CHECK(PlanOutput(in).first() == Tech::Reflex); // Reflex takes any input
    }

    // ---- ChooseInput ----
    {
        InputChoice in;
        CHECK(ChooseInput(in) == Input::None);

        // The UE mod, then OptiScaler's own while it runs, then the game's Reflex: each stands aside for the next
        in.ue = true;
        CHECK(ChooseInput(in) == Input::UeLowLatency);
        in.optiscalerRunning = true;
        CHECK(ChooseInput(in) == Input::OptiScaler);
        in.reflex = true;
        CHECK(ChooseInput(in) == Input::Reflex);

        // The game's XeLL and Anti-Lag 2 are inputs only with LOW_LATENCY_INPUTS
        InputChoice xell;
        xell.xell = xell.xellLowLatency = xell.xefgInput = true;
        xell.reflex = true;
        CHECK(ChooseInput(xell) == Input::Reflex);
        xell.lowLatencyInputs = true;
        CHECK(ChooseInput(xell) == Input::XeLL); // its XeFG paces with its XeLL
        xell.xefgInput = false;
        CHECK(ChooseInput(xell) == Input::Reflex);
        xell.reflex = false;
        CHECK(ChooseInput(xell) == Input::XeLL);
        xell.xellLowLatency = false;
        xell.antiLag2 = true;
        CHECK(ChooseInput(xell) == Input::AntiLag2);
        xell.antiLag2 = false;
        CHECK(ChooseInput(xell) == Input::XeLL); // low latency off: last, it does nothing until enabled

        InputChoice fsrFg;
        fsrFg.lowLatencyInputs = fsrFg.antiLag2 = fsrFg.antiLag2ReportsFg = fsrFg.reflex = true;
        CHECK(ChooseInput(fsrFg) == Input::AntiLag2); // the game's FSR-FG reports its frames only through it
    }

    // ---- OutputSwitchPending ----
    {
        // Not XeFG, or no output yet: nothing pending
        CHECK(!OutputSwitchPending(false, true, XefgLatency::Reflex, true));
        CHECK(!OutputSwitchPending(true, false, XefgLatency::Reflex, true));

        // The side and the output agree
        CHECK(!OutputSwitchPending(true, true, XefgLatency::XeLL, true));
        CHECK(!OutputSwitchPending(true, true, XefgLatency::Reflex, false));

        // The side moved, the output not yet
        CHECK(OutputSwitchPending(true, true, XefgLatency::Reflex, true));
        CHECK(OutputSwitchPending(true, true, XefgLatency::XeLL, false));

        // The UE mod on the XeLL side runs LatencyFlex: nothing pending
        CHECK(!OutputSwitchPending(true, true, XefgLatency::XeLL, false, false));
    }

    // ---- StreamlineCapUs ----
    {
        // Streamline of OptiScaler's DLSS FG output sets the device's mode too: it carries the output's cap
        CHECK(StreamlineCapUs(true, 16667) == 16667);
        CHECK(StreamlineCapUs(true, 0) == 0);
        CHECK(StreamlineCapUs(false, 16667) == 0);
    }

    // ---- SleepModeFor ----
    {
        const auto game = Mode(true, true, 5000);
        ShapeInputs in;

        // No XeFG: the game's own mode, the cap in place of its interval
        CHECK(Same(SleepModeFor(game, Role::Output, in), game));
        in.capIntervalUs = 13333;
        CHECK(Same(SleepModeFor(game, Role::Output, in), Mode(true, true, 13333)));

        // Force Disable without XeFG: the tech applies it, the mode stays
        in.forceDisabled = true;
        CHECK(Same(SleepModeFor(game, Role::Output, in), Mode(true, true, 13333)));
        in.forceDisabled = false;

        // A game with Reflex off: stays off without XeFG
        const auto gameOff = Mode(false, false, 0);
        CHECK(Same(SleepModeFor(gameOff, Role::Output, in), Mode(false, false, 13333)));

        // Input Off: low latency off, no Boost, no interval
        in.blocked = true;
        CHECK(Same(SleepModeFor(game, Role::Output, in), Mode(false, false, 0)));
        in.blocked = false;

        // XeFG with XeLL: low latency on whatever the game says, the cap in
        in.xefgOutput = true;
        CHECK(Same(SleepModeFor(gameOff, Role::Output, in), Mode(true, false, 13333)));

        // ... unless Force Disable
        in.forceDisabled = true;
        CHECK(Same(SleepModeFor(game, Role::Output, in), Mode(false, true, 13333)));
        in.forceDisabled = false;

        // ... Input Off wins
        in.blocked = true;
        CHECK(Same(SleepModeFor(game, Role::Output, in), Mode(false, false, 0)));
        in.blocked = false;

        // The split (Reflex side): the output gets the game's own with the cap, XeFG's XeLL everything off
        in.side = XefgLatency::Reflex;
        CHECK(Same(SleepModeFor(game, Role::Output, in), Mode(true, true, 13333)));
        CHECK(Same(SleepModeFor(gameOff, Role::Output, in), Mode(false, false, 13333)));
        CHECK(Same(SleepModeFor(game, Role::XefgXell, in), Mode(false, false, 0)));

        // Without a cap the game's interval stays for the side
        in.capIntervalUs = 0;
        CHECK(Same(SleepModeFor(game, Role::Output, in), game));

        // A game that only marks frames: Anti-Lag 2 sleeps on them with low latency on and the cap, Reflex gets its
        // (empty) mode
        in.capIntervalUs = 13333;
        in.inputSetMode = false;
        CHECK(Same(SleepModeFor(Mode(false, false, 0, false), Role::Output, in), Mode(false, false, 13333, false)));
        in.side = XefgLatency::AntiLag2;
        CHECK(Same(SleepModeFor(Mode(false, false, 0, false), Role::Output, in), Mode(true, false, 13333, false)));
        CHECK(Same(SleepModeFor(Mode(false, false, 0, false), Role::XefgXell, in), Mode(false, false, 0, false)));
    }

    // ---- CapForGame ----
    {
        GameCapInputs in;
        in.capFps = 75.0f;
        in.markersRecently = true;
        in.realNvapi = true;

        // No marker in 20 presents, or another frame generation owns Reflex: the limiter
        GameCapInputs quiet = in;
        quiet.markersRecently = false;
        quiet.gameSetSleepMode = true;
        CHECK(!CapForGame(quiet).outputHoldsCap);

        // Real Reflex, game set low latency on: Reflex keeps it at 75
        in.gameSetSleepMode = true;
        in.gameLowLatencyOn = true;
        auto c = CapForGame(in);
        CHECK(c.outputHoldsCap && c.fps == 75.0f && !c.showWarning);

        // FSR FG generating: halved, and the warning about real Reflex markers
        in.fsrFgOutput = true;
        in.fgActive = true;
        c = CapForGame(in);
        CHECK(c.outputHoldsCap && c.fps == 37.5f && c.showWarning);

        // ... with the game's Reflex off: the limiter keeps it
        in.gameLowLatencyOn = false;
        c = CapForGame(in);
        CHECK(!c.outputHoldsCap && !c.showWarning);
        in.fsrFgOutput = false;
        in.fgActive = false;

        // A game that never set a sleep mode, no XeFG: the limiter
        GameCapInputs markersOnly = in;
        markersOnly.gameSetSleepMode = false;
        markersOnly.gameSendingMarkers = true;
        CHECK(!CapForGame(markersOnly).outputHoldsCap);

        // XeFG with XeLL: XeLL keeps it while it holds caps (not idle, no Force Disable)
        in.xefgOutput = true;
        in.gameLowLatencyOn = true;
        in.xellHoldsCap = true;
        c = CapForGame(in);
        CHECK(c.outputHoldsCap && c.fps == 75.0f);
        in.xellHoldsCap = false;
        CHECK(!CapForGame(in).outputHoldsCap);

        // ... a game that only marks frames counts too
        markersOnly.xefgOutput = true;
        markersOnly.xellHoldsCap = true;
        CHECK(CapForGame(markersOnly).outputHoldsCap);

        // The split with NVIDIA Reflex: the game's own mode with low latency on, per rendered frame (2x: 37.5)
        in.side = XefgLatency::Reflex;
        in.xefgShownPerRendered = 2;
        c = CapForGame(in);
        CHECK(c.outputHoldsCap && c.fps == 37.5f);
        in.xefgShownPerRendered = 3;
        CHECK(CapForGame(in).fps == 25.0f);

        // ... not when the game's Reflex is off, nor for a game that only marks frames
        in.gameLowLatencyOn = false;
        CHECK(!CapForGame(in).outputHoldsCap);
        markersOnly.side = XefgLatency::Reflex;
        CHECK(!CapForGame(markersOnly).outputHoldsCap);

        // ... not on the D3D11 bridge (the driver did not hold it there)
        in.gameLowLatencyOn = true;
        in.splitLimiterKeepsCap = true;
        CHECK(!CapForGame(in).outputHoldsCap);
        in.splitLimiterKeepsCap = false;

        // Anti-Lag 2 (fakenvapi) acts on the markers alone
        markersOnly.realNvapi = false;
        markersOnly.side = XefgLatency::AntiLag2;
        CHECK(CapForGame(markersOnly).outputHoldsCap);

        // Input Off: never
        in.blocked = true;
        CHECK(!CapForGame(in).outputHoldsCap);
        in.blocked = false;

        // fakenvapi without XeFG and the game's DLSS FG at 3x: per rendered frame, except with XeLL
        GameCapInputs fake;
        fake.capFps = 120.0f;
        fake.markersRecently = true;
        fake.gameSetSleepMode = true;
        fake.dlssgFramesToGenerate = 2;
        c = CapForGame(fake);
        CHECK(c.outputHoldsCap && c.fps == 40.0f);
        fake.outputIsXell = true;
        CHECK(CapForGame(fake).fps == 120.0f);

        // fakenvapi keeps the cap even with the game's Reflex off
        fake.gameLowLatencyOn = false;
        fake.fgActive = true;
        CHECK(CapForGame(fake).outputHoldsCap);
    }

    static_assert(IntervalUs(75.0f) == 13333);
    static_assert(IntervalUs(37.5f) == 26667);
    static_assert(IntervalUs(0.0f) == 0);
    static_assert(PlanOutput(OutputInputs { true }).first() == Tech::Reflex);

    if (fails == 0)
        printf("ll_output_rule_smoke: PASS\n");

    return fails == 0 ? 0 : 1;
}
