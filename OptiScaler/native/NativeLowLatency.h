#pragma once

// Optical F5Low's low latency: OptiScaler calls the Reflex API itself, because a game that makes no upscaler call makes
// no Reflex call either, and XeFG's XeLL needs those calls in any game that does not make them. One path for every vendor: through ReflexHooks' function table, so real Reflex on NVIDIA and
// fakenvapi (Anti-Lag 2, XeLL or LatencyFlex, whichever the fakenvapi settings pick) everywhere else. D3D11 and D3D12
// go through the D3D-flavoured NVAPI entry points (NvAPI_D3D_*); a native Vulkan game goes through the Vulkan-flavoured
// ones (NvAPI_Vulkan_*, OnPresentBeginVulkan/OnPresentEndVulkan below) -- different device handle, different call
// surface, but the same decision and pacing underneath.
//
// The calls are paced by the game's Present, since there are no engine hooks:
//   after Present returns: PRESENT_END, Sleep, SIMULATION_START
//   the frame's first queue submit (D3D12) / first draw (D3D11) / nothing earlier on Vulkan: SIMULATION_END,
//   RENDERSUBMIT_START
//   just before Present: RENDERSUBMIT_END, PRESENT_START
// The markers are approximate, so useMarkersToOptimize stays off. It runs only while native/NativeLowLatencyRule.h
// says so (an Optical F5Low mode or XeFG runs, the game makes no Reflex call itself, fakenvapi's Force Reflex and Force
// XeLL are not in the way) and stands aside the moment the game calls Reflex. Our own calls are not counted as the game's.
// Never writes a fakenvapi setting.

#include "NativeLowLatencyRule.h"

#include <Unknwn.h>
#include <vulkan/vulkan.h>

namespace native::lowlatency
{
// The game's Present: before the real Present (RENDERSUBMIT_END, PRESENT_START) and after it returns (PRESENT_END,
// Sleep, SIMULATION_START). `device` is the game's D3D12 or D3D11 device. SwapChain calls are ignored while
// FrameGeneration calls come in.
void OnPresentBegin(IUnknown* device, PresentSource source = PresentSource::SwapChain);
void OnPresentEnd(IUnknown* device, PresentSource source = PresentSource::SwapChain);

// Same pairing, for a native Vulkan game (native/NativeDriverVk.cpp, around the game's own vkQueuePresentKHR). There is
// no earlier queue-submit hook on this path either, so OnPresentBeginVulkan's own fallback (as OnPresentBegin's) sends
// SIMULATION_END/RENDERSUBMIT_START too, right before RENDERSUBMIT_END/PRESENT_START: no OnFirstSubmit equivalent
// needed. Calls the Vulkan-flavoured Reflex entry points (NvAPI_Vulkan_*), never the D3D ones, even when a D3D12 device
// happens to share this process (dxvk): the two share no state. While the Vulkan present bridge is up the game's present
// is paced on the bridge's private D3D12 device through OnPresentBegin/OnPresentEnd instead (hooks/Vulkan_Hooks.cpp,
// NativeLowLatencyRule.h PresentTarget), so XeFG's XeLL routing applies; each call carries its own API.
void OnPresentBeginVulkan(VkDevice device);
void OnPresentEndVulkan(VkDevice device);

// The frame's first queue submit (D3D12) or first draw (D3D11): any thread, one relaxed load when we do not run.
// `queue`: the D3D12 queue submitted to; only the game's queue counts. nullptr (D3D11) always counts.
void OnFirstSubmit(const void* queue = nullptr);

// The game's device is being released for good: nothing of ours may use it again.
void OnDeviceReleased(IUnknown* device);
void OnDeviceReleasedVulkan(VkDevice device);

struct Status
{
    Decision decision = Decision::NoF5Low;
    const char* path = ""; // plain words for the method in use, empty unless decision is Run
    bool hasLatency = false;
    float latencyMs = 0.0f; // the game's simulation start to the end of the GPU work, from GetLatency
    CapOwner capOwner = CapOwner::NotOurs;
    bool xellIdle = false; // XeLL on an NVIDIA/AMD GPU with no XeFG generating frames: it does nothing
    bool forXefgOnly = false; // runs for XeFG's XeLL alone (Auto, no Optical F5Low mode)
    // The game's own Reflex calls are sent to XeFG's XeLL (bridged Vulkan game): DecisionText says so
    bool gameReflexFeedsXell = false;
};

// The menu's line. Cheap; GetLatency is asked at most twice a second.
Status GetStatus();

// We run (markers and Sleep go out). Any thread, two relaxed loads.
bool Running();

// We run and our SetSleepMode carries the frame cap (NativeLowLatencyRule.h PlanCap): OptiScaler's own limiter
// (misc/FrameLimit) must not wait as well. Any thread, one relaxed load.
bool OwnsFrameCap();

// Our frame in flight's XeLL frame ID while XeFG runs on our XeLL, for XeFG to tag and present it with (Intel: one ID
// for both), else 0. Any thread.
uint32_t FrameIdForFrameGeneration();

// DLSS frame generation runs Reflex itself through Streamline (Decision::FrameGenerationOwnsReflex) and the player
// asked for Boost: framegen/dlssg sets Streamline's Reflex mode with Boost.
bool WantsBoostForFrameGeneration();

// Intel's XeLL guide: all GPU work finished before xellSetSleepMode (R11). Drains the game's D3D12 queue (frame
// generation's game queue, else the current one; the bridges' D3D12 queue for D3D11 and Vulkan games), 250 ms at most.
// Called by fakenvapi's XeLL right before it changes the sleep mode, whoever asked (our low latency, the game's
// Reflex, the frame cap). Any thread.
void WaitForGameQueueIdle();
} // namespace native::lowlatency
