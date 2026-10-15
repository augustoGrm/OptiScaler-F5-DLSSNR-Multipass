#pragma once

// The in-upscaler menu's lifetime (the old overlay: Menu_Dx12, drawn inside a D3D12 feature's Evaluate).
//
// One menu is shared by every feature and outlives them. A feature going away (a rebuild, a backend switch) leaves it:
// the next feature has been drawing with it, and those lists may still be on the GPU. Freeing it then hung RE2 on AMD
// (DXGI_ERROR_DEVICE_HUNG 27 ms after the old feature's delayed destructor freed it).
//
// It is replaced only when it no longer fits: another window, device or output format (its pipeline is made for one).
// Then it is retired: it stops drawing, is destroyed after the delay a destroyed feature gets (Util::DelayedDestroy),
// and only then is a new one made. ImGui's DX12 backend and the menu's SRV heap allocator are globals, so two menus
// must never be alive at once. The last feature going retires it the same way, as freeing it did before.
namespace menulife
{
enum class Step
{
    Draw,   // draw with the menu there is
    Create, // none: make one
    Retire, // it no longer fits: take it out of use, destroyed after the delay
    Wait,   // a retired one is still going: neither draw nor make one
};

struct Inputs
{
    bool hasMenu = false;
    bool retiring = false; // a retired menu has not been destroyed yet
    bool fits = true;      // same window, device and output format (read only with a menu)
};

inline Step Next(const Inputs& in)
{
    if (in.retiring)
        return Step::Wait;

    if (!in.hasMenu)
        return Step::Create;

    return in.fits ? Step::Draw : Step::Retire;
}

// A feature's destructor: the menu goes with the last live feature only
inline bool RetireOnFeatureGone(int liveFeaturesAfter) { return liveFeaturesAfter <= 0; }
} // namespace menulife
