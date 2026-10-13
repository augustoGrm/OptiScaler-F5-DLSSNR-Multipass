// Host check of native/VkHiddenSurfaceRule.h: when the Vulkan bridge makes its hidden window and surface again. No GPU,
// no game. cl /std:c++20 /EHsc /W4 tests/nr_vk_hidden_surface_rule_smoke.cpp
#include "../OptiScaler/native/VkHiddenSurfaceRule.h"

#include <cstdio>

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

// The bridge's loop (VkPresentBridge.cpp OnCreateSwapchain): `patchResults` answers each Patch in turn, `remakeWorks`
// each RemakeSurface. Returns the last step and counts the remakes.
static HiddenSurfaceStep Run(const bool* patchResults, int patches, bool gameSwapchainOnSurface, bool remakeWorks,
                             int* remakes)
{
    bool remade = false;
    *remakes = 0;

    for (int i = 0; i < patches; ++i)
    {
        const auto step = OnHiddenSurfacePatch(patchResults[i], gameSwapchainOnSurface, remade);

        if (step != HiddenSurfaceStep::Remake)
            return step;

        ++*remakes;
        remade = true;

        if (!remakeWorks)
            return HiddenSurfaceStep::Retire;
    }

    return HiddenSurfaceStep::Retire;
}

int main()
{
    // The swapchain fits: used, whatever else is true
    CHECK(OnHiddenSurfacePatch(true, false, false) == HiddenSurfaceStep::Use);
    CHECK(OnHiddenSurfacePatch(true, true, false) == HiddenSurfaceStep::Use);
    CHECK(OnHiddenSurfacePatch(true, false, true) == HiddenSurfaceStep::Use);

    // It does not: made again only with no game swapchain on the surface and not made again already
    CHECK(OnHiddenSurfacePatch(false, false, false) == HiddenSurfaceStep::Remake);
    CHECK(OnHiddenSurfacePatch(false, true, false) == HiddenSurfaceStep::Retire);
    CHECK(OnHiddenSurfacePatch(false, false, true) == HiddenSurfaceStep::Retire);
    CHECK(OnHiddenSurfacePatch(false, true, true) == HiddenSurfaceStep::Retire);

    int remakes = 0;

    // Lost surface, made again, fits: used after one remake (Arkham after leaving full screen)
    {
        const bool answers[] = { false, true };
        CHECK(Run(answers, 2, false, true, &remakes) == HiddenSurfaceStep::Use && remakes == 1);
    }

    // Still failing after the remake: retired, never a second remake
    {
        const bool answers[] = { false, false, false, false };
        CHECK(Run(answers, 4, false, true, &remakes) == HiddenSurfaceStep::Retire && remakes == 1);
    }

    // The remake itself fails: retired
    {
        const bool answers[] = { false, true };
        CHECK(Run(answers, 2, false, false, &remakes) == HiddenSurfaceStep::Retire && remakes == 1);
    }

    // A game swapchain still lives on the surface: no remake (the surface must outlive it)
    {
        const bool answers[] = { false, true };
        CHECK(Run(answers, 2, true, true, &remakes) == HiddenSurfaceStep::Retire && remakes == 0);
    }

    if (fails == 0)
        printf("nr_vk_hidden_surface_rule_smoke: PASS\n");

    return fails == 0 ? 0 : 1;
}
