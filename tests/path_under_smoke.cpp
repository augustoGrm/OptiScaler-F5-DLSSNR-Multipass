// Host check of PathUnder.h: Util::IsSubpath's spelling rule (Streamline and plugin folders, the driver store).
// No GPU and no game needed.
// cl /std:c++20 /EHsc /W4 tests/path_under_smoke.cpp
#include "../OptiScaler/misc/PathUnder.h"

#include <cstdio>

using pathrule::IsUnder;

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
    // Below, at any depth
    CHECK(IsUnder(L"C:\\Games\\X\\streamline\\sl.dlss_g.dll", L"C:\\Games\\X\\streamline"));
    CHECK(IsUnder(L"C:\\Games\\X\\a\\b\\c.dll", L"C:\\Games\\X"));

    // Case: Windows paths match whatever the spelling (upstream's 880402ba case)
    CHECK(IsUnder(L"c:\\games\\x\\Streamline\\sl.interposer.dll", L"C:\\Games\\X\\streamline"));
    CHECK(IsUnder(L"C:\\GAMES\\X\\STREAMLINE\\A.DLL", L"c:\\games\\x\\streamline"));

    // Separators and spelling: forward slashes, a trailing separator, `.` and `..`
    CHECK(IsUnder(L"C:/Games/X/streamline/a.dll", L"C:\\Games\\X\\streamline\\"));
    CHECK(IsUnder(L"C:\\Games\\X\\.\\streamline\\..\\streamline\\a.dll", L"C:\\Games\\X\\streamline"));

    // Not below: the base itself, a sibling, a sibling sharing a prefix, a parent, another drive, empty
    CHECK(!IsUnder(L"C:\\Games\\X\\streamline", L"C:\\Games\\X\\streamline"));
    CHECK(!IsUnder(L"C:\\Games\\X\\streamline\\", L"C:\\Games\\X\\streamline"));
    CHECK(!IsUnder(L"C:\\Games\\X\\other\\a.dll", L"C:\\Games\\X\\streamline"));
    CHECK(!IsUnder(L"C:\\Games\\X\\streamline2\\a.dll", L"C:\\Games\\X\\streamline"));
    CHECK(!IsUnder(L"C:\\Games\\X", L"C:\\Games\\X\\streamline"));
    CHECK(!IsUnder(L"D:\\Games\\X\\streamline\\a.dll", L"C:\\Games\\X\\streamline"));
    CHECK(!IsUnder(L"C:\\Games\\X\\streamline\\..\\other\\a.dll", L"C:\\Games\\X\\streamline"));
    CHECK(!IsUnder(L"C:\\a.dll", L""));

    // The driver store: a DLL there and one beside it
    const wchar_t* store = L"C:\\Windows\\System32\\DriverStore\\FileRepository\\nv_dispi.inf_amd64_abc";
    CHECK(
        IsUnder(L"c:\\windows\\system32\\driverstore\\filerepository\\NV_DISPI.INF_AMD64_ABC\\nvngx_dlssg.dll", store));
    CHECK(!IsUnder(L"C:\\Windows\\System32\\nvngx_dlssg.dll", store));

    if (fails == 0)
        printf("path_under_smoke: all checks passed\n");

    return fails == 0 ? 0 : 1;
}
