#include "../OptiScaler/framegen/dlssg/Sm86Control.h"
#include <cassert>
#include <cstdio>

int main()
{
    using namespace AmpereMfgLoader::Control;
    const auto follow = MakeRequest(0, false, 0);
    assert(follow.mode == Mode::FollowGame && follow.multiplier == 0);
    for (int factor = 2; factor <= 6; ++factor)
    {
        const auto fixed = MakeRequest(factor, false, 144);
        assert(fixed.mode == Mode::Fixed && fixed.multiplier == (uint32_t)factor);
        assert(fixed.targetFps == 144 && fixed.version == 1 && fixed.size == 20);
    }
    const auto dynamic = MakeRequest(4, true, 120);
    assert(dynamic.mode == Mode::Dynamic && dynamic.multiplier == 0 && dynamic.targetFps == 120);
    assert(MakeRequest(-5, false, -1).mode == Mode::FollowGame);
    assert(MakeRequest(200, false, 5000).multiplier == 6);
    assert(MakeRequest(200, false, 5000).targetFps == 1000);
    std::puts("sm86_control_smoke: follow, X2-X6, dynamic and bounds passed");
}
