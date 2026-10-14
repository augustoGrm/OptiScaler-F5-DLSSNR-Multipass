#pragma once

// dxvk lists a GPU's outputs in the order Windows enumerates the monitors (EnumDisplayMonitors), while Windows' own
// DXGI always lists the primary display first. Games go full screen on output 0 and check their resolution against its
// modes, so with a monitor ordered before the primary one (a portrait monitor to its left, say) a D3D11 game on dxvk
// opens on that monitor, and a resolution it does not have falls back to one it has (Batman: Arkham Knight saved
// 480x720 over 2560x1440). Once a dxvk adapter is seen, its EnumOutputs is detoured to put the primary display first,
// the rest in their order, as Windows' DXGI does. Only dxvk's method is detoured; Windows' DXGI is not touched.

#include <dxgi.h>

#include <cstdint>

namespace DxvkOutputs
{

// Detours dxvk's IDXGIAdapter::EnumOutputs through this adapter's vtable, once (later calls do nothing). The adapter
// must be dxvk's (it answers IDXGIVkInteropAdapter); others are left alone.
void Hook(IUnknown* adapter);

// The output dxvk is asked for when the game asks for `requested`, with dxvk's primary display at `primary` (negative:
// none found): the primary first, then the others in dxvk's order.
inline uint32_t Remap(uint32_t requested, int primary)
{
    if (primary <= 0)
        return requested;

    if (requested == 0)
        return (uint32_t) primary;

    return requested <= (uint32_t) primary ? requested - 1 : requested;
}

} // namespace DxvkOutputs
