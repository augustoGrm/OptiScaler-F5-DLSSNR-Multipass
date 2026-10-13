#pragma once

#include <mutex>

namespace native
{
// One lock for the calls that make the NVIDIA driver start its Vulkan and CUDA sides at once.
//
// Seen frozen in Assassin's Creed Odyssey on dxvk: the game's startup thread was inside vkCreateDevice (holding the
// driver's Vulkan lock, waiting for a CUDA lock) while the render thread was in DLSS-NR's feature creation (holding the
// CUDA lock, waiting for the Vulkan lock). The driver takes the two in opposite orders, so two threads that are in each
// at the same moment never come out. Our two entry points -- the hooked vkCreateDevice and the model's feature create --
// take this first, so they never overlap. Recursive: the create can reach a hooked vkCreateDevice on its own thread.
inline std::recursive_mutex& DriverInitMutex()
{
    static std::recursive_mutex mutex;
    return mutex;
}

class DriverInitScope
{
  public:
    DriverInitScope() : _lock(DriverInitMutex()) {}

  private:
    std::lock_guard<std::recursive_mutex> _lock;
};
} // namespace native
