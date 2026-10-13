// Host check of UeInputRule.h: the UE Low Latency mod as a low latency input. No GPU and no game needed.
// cl /std:c++20 /EHsc /W4 tests/ue_low_latency_input_smoke.cpp
#include "../OptiScaler/low_latency/input/UeInputRule.h"

#include <cstdio>

using namespace ueinput;

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
    // Only one sleeper: the mod drives the output only when neither the game's Reflex nor ours runs
    CHECK(!StandAside(false, false));
    CHECK(StandAside(true, false));
    CHECK(StandAside(false, true));
    CHECK(StandAside(true, true));

    // First frame: always sends, on unless Input Off
    CHECK(SleepModeToSend(false, std::nullopt) == std::optional<bool>(true));
    CHECK(SleepModeToSend(true, std::nullopt) == std::optional<bool>(false));

    // Nothing while it holds
    CHECK(!SleepModeToSend(false, true).has_value());
    CHECK(!SleepModeToSend(true, false).has_value());

    // Input Off and back: one send each way
    CHECK(SleepModeToSend(true, true) == std::optional<bool>(false));
    CHECK(SleepModeToSend(false, false) == std::optional<bool>(true));

    // Every rule is constexpr
    static_assert(StandAside(false, true));
    static_assert(SleepModeToSend(false, true) == std::nullopt);

    if (fails == 0)
        printf("ue_low_latency_input_smoke: PASS\n");

    return fails == 0 ? 0 : 1;
}
