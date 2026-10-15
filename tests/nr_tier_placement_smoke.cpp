// Host check of dlssnr/DlssNr_TierPlacement.h: the resolution tiers (High to Potato) set the model's size and method, and
// leave "NR Pass at: Finished Picture" alone, so NR Only mode keeps running when a tier is picked.
// cl /std:c++20 /EHsc /W4 tests/nr_tier_placement_smoke.cpp
#include "../OptiScaler/dlssnr/DlssNr_TierPlacement.h"

#include <cstdio>
#include <initializer_list>

using namespace DlssNrTierPlacement;

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
    // After SR / before SR: a tier row moves NR to its own placement, as it always did
    {
        const Placement afterSr { false, false, false };
        const auto post = AfterTier(afterSr, false);
        CHECK(!post.finishedPicture && !post.runBeforeSr && !post.deferred);

        const auto pre = AfterTier(afterSr, true);
        CHECK(!pre.finishedPicture && pre.runBeforeSr && !pre.deferred);

        const Placement deferred { false, true, true };
        const auto fromDeferred = AfterTier(deferred, false);
        CHECK(!fromDeferred.finishedPicture && !fromDeferred.runBeforeSr && !fromDeferred.deferred);
    }

    // Finished Picture (NR Only, and NR + frame generation's picture): any tier row keeps the placement as it is
    for (bool beforeRow : { false, true })
        for (bool runBeforeSr : { false, true })
            for (bool deferred : { false, true })
            {
                const Placement finished { true, runBeforeSr, deferred };
                const auto after = AfterTier(finished, beforeRow);
                CHECK(after.finishedPicture);
                CHECK(after.runBeforeSr == runBeforeSr);
                CHECK(after.deferred == deferred);
            }

    // The highlight: after/before SR needs the row's placement; Finished Picture matches both rows (placement does not
    // apply there)
    CHECK(RowMatchesPlacement({ false, false, false }, false));
    CHECK(!RowMatchesPlacement({ false, false, false }, true));
    CHECK(RowMatchesPlacement({ false, true, false }, true));
    CHECK(!RowMatchesPlacement({ false, true, false }, false));
    CHECK(RowMatchesPlacement({ true, false, false }, false));
    CHECK(RowMatchesPlacement({ true, true, true }, true));

    // Picking a tier and then asking which row is lit is consistent, for every start
    for (bool finished : { false, true })
        for (bool beforeRow : { false, true })
        {
            const auto after = AfterTier({ finished, false, false }, beforeRow);
            CHECK(RowMatchesPlacement(after, beforeRow));
        }

    if (fails == 0)
        printf("PASS\n");

    return fails == 0 ? 0 : 1;
}
