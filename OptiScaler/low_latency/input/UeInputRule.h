#pragma once

// The UE Low Latency mod as a low latency input (input_uell.cpp): when its frames drive the output, and which sleep
// mode it sends. Pure, host tested (tests/ue_low_latency_input_smoke.cpp).

#include <optional>

namespace ueinput
{
// One sleeper per frame: the game's own Reflex, or OptiScaler's own low latency (Optical F5Low, frame generation),
// already lowers latency, so the mod's frames go nowhere
constexpr bool StandAside(bool gameCalledReflex, bool ownLowLatencyRuns)
{
    return gameCalledReflex || ownLowLatencyRuns;
}

// The sleep mode to send this frame (low latency on or off), nothing while the last one sent still holds. The Framerate
// page's Input Off sends low latency off; Sleep and the markers keep flowing.
constexpr std::optional<bool> SleepModeToSend(bool inputOff, std::optional<bool> lastSent)
{
    const bool lowLatency = !inputOff;

    if (lastSent == lowLatency)
        return std::nullopt;

    return lowLatency;
}
} // namespace ueinput
