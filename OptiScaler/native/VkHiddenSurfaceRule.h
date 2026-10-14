#pragma once

// What the Vulkan bridge does when the game's new swapchain cannot go on the hidden surface (its capabilities query
// fails: Batman: Arkham Knight on dxvk, after leaving exclusive full screen). A new hidden window and surface can only
// be made while no game swapchain lives on the old one (a surface must outlive every swapchain made on it), and only
// once per swapchain the game makes, so a surface that keeps failing ends in the bridge being retired instead of a
// loop. Header-only and free of D3D and Vulkan: a host test includes it.

namespace native
{
enum class HiddenSurfaceStep
{
    Use,    // the swapchain goes on the hidden surface
    Remake, // make the hidden window and surface again, then try once more
    Retire  // the bridge is retired; the game's swapchain goes on its own window
};

// `patched`: the swapchain fits the hidden surface. `gameSwapchainOnSurface`: one of the game's swapchains still lives
// on it. `remade`: the hidden surface was already made again for this swapchain.
inline constexpr HiddenSurfaceStep OnHiddenSurfacePatch(bool patched, bool gameSwapchainOnSurface, bool remade)
{
    if (patched)
        return HiddenSurfaceStep::Use;

    if (gameSwapchainOnSurface || remade)
        return HiddenSurfaceStep::Retire;

    return HiddenSurfaceStep::Remake;
}
} // namespace native
