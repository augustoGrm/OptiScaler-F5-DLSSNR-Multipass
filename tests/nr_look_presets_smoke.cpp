// Host test for OptiScaler/dlssnr/DlssNr_LookPresets.h: the menu's look presets.
// cl /std:c++20 /EHsc /W4 tests/nr_look_presets_smoke.cpp
#include "../OptiScaler/dlssnr/DlssNr_LookPresets.h"

#include <cstdio>
#include <cstring>

using namespace DlssNrLook;

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
    CHECK(kLookCount == 7);

    // Every look reads as itself, and as no other (two looks never show green together)
    for (size_t i = 0; i < kLookCount; ++i)
    {
        CHECK(Matches(kLooks[i].settings, kLooks[i].settings));

        for (size_t j = 0; j < kLookCount; ++j)
            if (i != j)
                CHECK(!Matches(kLooks[i].settings, kLooks[j].settings));

        CHECK(kLooks[i].name != nullptr && kLooks[i].name[0] != '\0');
        CHECK(kLooks[i].tip != nullptr && kLooks[i].tip[0] != '\0');
    }

    // Any owned value changed: the look is no longer in effect
    {
        const Settings& base = kLooks[0].settings;
        auto changed = [&](auto edit)
        {
            Settings s = base;
            edit(s);
            return !Matches(s, base);
        };

        CHECK(changed([](Settings& s) { s.pass1.style = 2; }));
        CHECK(changed([](Settings& s) { s.pass1.intensity += 0.1f; }));
        CHECK(changed([](Settings& s) { s.pass1.structure += 0.1f; }));
        CHECK(changed([](Settings& s) { s.pass1.tone += 0.1f; }));
        CHECK(changed([](Settings& s) { s.pass1.skin += 0.1f; }));
        CHECK(changed([](Settings& s) { s.pass1.autoMask = !s.pass1.autoMask; }));
        CHECK(changed([](Settings& s) { s.pass2 = Profile {}; }));
        CHECK(changed([](Settings& s) { s.laterOverrides = true; }));
        CHECK(changed([](Settings& s) { s.curve = 5; }));
        CHECK(changed([](Settings& s) { s.detail = 1.5f; }));
        CHECK(changed([](Settings& s) { s.colour = 0.5f; }));
        CHECK(changed([](Settings& s) { s.skinSplit = true; }));
        CHECK(changed([](Settings& s) { s.skinTone = false; }));
        CHECK(changed([](Settings& s) { s.skinDetail = 0.5f; }));
        CHECK(changed([](Settings& s) { s.skinColour = 0.5f; }));
        CHECK(changed([](Settings& s) { s.envDetail = 0.5f; }));
        CHECK(changed([](Settings& s) { s.envColour = 0.5f; }));
        CHECK(changed([](Settings& s) { s.inputStyle = 3; }));
        CHECK(changed([](Settings& s) { s.inputStrength = 0.5f; }));
        CHECK(changed([](Settings& s) { s.facesOnly = true; }));

        // Within the sliders' rounding it still matches
        Settings s = base;
        s.pass1.intensity += 0.001f;
        CHECK(Matches(s, base));
    }

    // The values the user chose
    {
        const Settings& natural = Get(Look::Natural).settings;
        CHECK(natural.pass1.style == 1 && natural.pass1.intensity == 1.4f && natural.pass1.structure == 1.4f &&
              natural.pass1.tone == 1.4f);
        CHECK(natural.pass2.has_value() && natural.pass3.has_value());
        CHECK(natural.curve == kCurveHlg);

        const Settings& native = Get(Look::Native).settings;
        CHECK(native.curve == kCurveLinear && native.pass1.tone == 0.0f && native.pass1.skin == 0.75f);
        CHECK(!native.pass2.has_value() && !native.pass3.has_value());

        const Settings& soft = Get(Look::SoftCanvas).settings;
        CHECK(soft.inputStyle == kInputSmoothCanvas && soft.curve == kCurveHlg && soft.detail == 2.0f);

        CHECK(Get(Look::DetailOnly).settings.colour == 0.0f);
        CHECK(Get(Look::Painterly).settings.inputStyle == kInputWatercolour &&
              Get(Look::Painterly).settings.detail == 1.5f);
        CHECK(Get(Look::Faces).settings.skinSplit && Get(Look::Faces).settings.envDetail < 1.0f);
        CHECK(Get(Look::Cinematic).settings.pass1.style == 2);

        CHECK(Get(Look::Painterly).experimental && Get(Look::SoftCanvas).experimental);
        CHECK(!Get(Look::Native).experimental && !Get(Look::Natural).experimental);
    }

    // Tiers make the composition composed and leave a composed curve alone, so a look stays in effect after one
    CHECK(TierCurve(kCurveReplace) == kCurveComposed);
    CHECK(TierCurve(kCurveReplaceBalanced) == kCurveComposed);

    for (uint32_t curve = 0; curve <= 7; ++curve)
        CHECK(IsReplace(curve) || TierCurve(curve) == curve);

    for (size_t i = 0; i < kLookCount; ++i)
    {
        CHECK(!IsReplace(kLooks[i].settings.curve));
        Settings after = kLooks[i].settings;
        after.curve = TierCurve(after.curve);
        CHECK(Matches(after, kLooks[i].settings));
    }

    if (fails != 0)
    {
        printf("look presets: %d failure(s)\n", fails);
        return 1;
    }

    printf("look presets: ok\n");
    return 0;
}
