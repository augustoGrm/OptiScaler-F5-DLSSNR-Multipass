// Host check of native/VirtualUpscalerMode.h: what Optical F5Low's virtual upscaler does with Neural Rendering, by the
// ini switch NativeUpscalerPass (auto, off, on). cl /std:c++20 /EHsc /W4 tests/f5low_vu_route_smoke.cpp
#include "../OptiScaler/native/VirtualUpscalerMode.h"

#include <cstdio>
#include <initializer_list>

using namespace native;

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
    // NR off: nothing runs on the picture, whatever the switch or the screen says
    for (bool pq : { false, true })
        for (auto setting : { VuPassSetting::Auto, VuPassSetting::Off, VuPassSetting::On })
            CHECK(DecideVuNrRoute(false, pq, setting) == VuNrRoute::None);

    // NR on, SDR or scRGB: on the game's picture, unless the switch asks for the backend's pass
    CHECK(DecideVuNrRoute(true, false, VuPassSetting::Auto) == VuNrRoute::InPlace);
    CHECK(DecideVuNrRoute(true, false, VuPassSetting::Off) == VuNrRoute::InPlace);
    CHECK(DecideVuNrRoute(true, false, VuPassSetting::On) == VuNrRoute::BackendPass);

    // HDR10 (PQ): native input has no path for it yet, so the backend's pass stays, whatever the switch says
    for (auto setting : { VuPassSetting::Auto, VuPassSetting::Off, VuPassSetting::On })
        CHECK(DecideVuNrRoute(true, true, setting) == VuNrRoute::BackendPass);

    // The ini value: unset is auto, false is off, true is on
    CHECK(VuPassSettingFromIni(false, false) == VuPassSetting::Auto);
    CHECK(VuPassSettingFromIni(false, true) == VuPassSetting::Auto); // an unset key holds no value to read
    CHECK(VuPassSettingFromIni(true, false) == VuPassSetting::Off);
    CHECK(VuPassSettingFromIni(true, true) == VuPassSetting::On);

    if (fails == 0)
        printf("PASS\n");

    return fails == 0 ? 0 : 1;
}
