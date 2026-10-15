// Host test for OptiScaler/menu/MenuLifetimeRule.h: the in-upscaler menu (old overlay) outlives the features that draw
// it. Build (VS x64 prompt, repo root): cl /std:c++20 /EHsc /W4 tests/menu_lifetime_smoke.cpp
#include "../OptiScaler/menu/MenuLifetimeRule.h"

#include <cstdio>

static int g_failures = 0;

#define CHECK(c)                                                                                                       \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(c))                                                                                                      \
        {                                                                                                              \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);                                                   \
            ++g_failures;                                                                                              \
        }                                                                                                              \
    } while (0)

using namespace menulife;

// A model of IFeature_Dx12's use of the rule: the shared menu, the retiring one and the live features
struct Model
{
    bool menu = false;
    bool retiring = false;
    int features = 0;
    int menusMade = 0;
    int menusAlive = 0; // never above 1: ImGui's DX12 backend and the SRV heap allocator are globals
    int maxAlive = 0;

    Step Frame(bool fits = true)
    {
        Inputs in;
        in.hasMenu = menu;
        in.retiring = retiring;
        in.fits = fits;
        const Step step = Next(in);

        switch (step)
        {
        case Step::Create:
            menu = true;
            ++menusMade;
            ++menusAlive;
            break;
        case Step::Retire:
            menu = false;
            retiring = true;
            break;
        default:
            break;
        }

        if (menusAlive > maxAlive)
            maxAlive = menusAlive;

        return step;
    }

    void FeatureMade() { ++features; }

    // A feature's destructor (after its own delay)
    void FeatureGone()
    {
        if (RetireOnFeatureGone(--features) && menu)
        {
            menu = false;
            retiring = true;
        }
    }

    // The retired menu's delay is over: it is destroyed
    void RetiredGone()
    {
        retiring = false;
        --menusAlive;
    }
};

int main()
{
    // The first frame makes the menu, the next ones draw with it
    {
        Model m;
        m.FeatureMade();
        CHECK(m.Frame() == Step::Create);
        CHECK(m.Frame() == Step::Draw);
    }

    // The RE2 case: the virtual upscaler rebuilds (new feature first, the old one destroyed 2 s later). The old one
    // going leaves the menu the new one draws with, however many times it happens.
    {
        Model m;
        m.FeatureMade();
        m.Frame();

        for (int rebuild = 0; rebuild < 3; ++rebuild)
        {
            m.FeatureMade(); // the new backend
            CHECK(m.Frame() == Step::Draw);
            m.FeatureGone(); // the old backend's delayed destructor
            CHECK(!m.retiring);
            CHECK(m.Frame() == Step::Draw);
        }

        CHECK(m.menusMade == 1);
    }

    // A game feature and the virtual upscaler side by side: either going leaves the menu
    {
        Model m;
        m.FeatureMade();
        m.FeatureMade();
        m.Frame();
        m.FeatureGone();
        CHECK(m.Frame() == Step::Draw);
    }

    // Another window, device or output format: retire, draw nothing and make nothing until the old one is gone,
    // then make the new one. Never two alive.
    {
        Model m;
        m.FeatureMade();
        m.Frame();
        CHECK(m.Frame(false) == Step::Retire);
        CHECK(m.Frame() == Step::Wait);
        CHECK(m.Frame(false) == Step::Wait);
        CHECK(m.menusAlive == 1);
        m.RetiredGone();
        CHECK(m.Frame() == Step::Create);
        CHECK(m.Frame() == Step::Draw);
        CHECK(m.menusMade == 2);
        CHECK(m.maxAlive == 1);
    }

    // The last feature gone (the game stops upscaling or exits): the menu goes too, as before, by the same delay
    {
        Model m;
        m.FeatureMade();
        m.Frame();
        m.FeatureGone();
        CHECK(!m.menu && m.retiring);

        // A feature made while it is still going waits for it
        m.FeatureMade();
        CHECK(m.Frame() == Step::Wait);
        m.RetiredGone();
        CHECK(m.Frame() == Step::Create);
        CHECK(m.maxAlive == 1);
    }

    CHECK(!RetireOnFeatureGone(1));
    CHECK(RetireOnFeatureGone(0));

    if (g_failures != 0)
    {
        std::printf("menu_lifetime_smoke: %d failure(s)\n", g_failures);
        return 1;
    }

    std::printf("menu_lifetime_smoke: PASS\n");
    return 0;
}
