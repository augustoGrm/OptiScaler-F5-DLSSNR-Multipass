// Host test for OptiScaler/hooks/DxvkOutputs.h: the outputs a game sees through dxvk, with the primary display first and
// the rest in dxvk's order, as Windows' DXGI lists them.
//
//   cl /std:c++20 /EHsc /W4 /I OptiScaler tests\dxvk_output_order_smoke.cpp

#include "hooks/DxvkOutputs.h"

#include <cstdio>
#include <string>
#include <vector>

namespace
{

int g_failures = 0;

// What the game sees for outputs 0..n-1 when dxvk lists `dxvk` and the primary display is at `primary`.
std::string Seen(const std::vector<std::string>& dxvk, int primary)
{
    std::string out;

    for (uint32_t i = 0; i < dxvk.size(); ++i)
        out += (i ? " " : "") + dxvk[DxvkOutputs::Remap(i, primary)];

    return out;
}

void Expect(const char* what, const std::string& got, const std::string& want)
{
    const bool ok = got == want;
    printf("  %-58s %-22s %s\n", what, got.c_str(), ok ? "ok" : ("FAIL, want " + want).c_str());
    g_failures += ok ? 0 : 1;
}

} // namespace

int main()
{
    // The machine this was found on: a portrait monitor left of the primary is dxvk's output 0. Windows' DXGI lists
    // DISPLAY2 DISPLAY1 DISPLAY3.
    Expect("primary second (portrait monitor to its left)", Seen({ "D1", "D2", "D3" }, 1), "D2 D1 D3");
    Expect("primary last", Seen({ "D1", "D2", "D3" }, 2), "D3 D1 D2");
    Expect("primary already first: unchanged", Seen({ "D1", "D2", "D3" }, 0), "D1 D2 D3");
    Expect("no primary on this GPU: unchanged", Seen({ "D1", "D2" }, -1), "D1 D2");
    Expect("one monitor", Seen({ "D1" }, 0), "D1");

    // Past the last output the game gets dxvk's own answer (not found).
    Expect("past the last output stays past it", std::to_string(DxvkOutputs::Remap(3, 1)), "3");

    printf(g_failures == 0 ? "all passed\n" : "%d FAILED\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
