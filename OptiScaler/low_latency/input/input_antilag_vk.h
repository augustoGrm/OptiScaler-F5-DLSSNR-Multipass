#pragma once

#include "input_common.h"

#include <vulkan/vulkan.h>

// The game's AntiLag 2 for Vulkan through VK_AMD_anti_lag (it needs the driver's extension): the Anti-Lag 2 input on
// its VkDevice, chosen last as the game's Anti-Lag 2 is (OutputRule.h ChooseInput). Handed out by VulkanHooks.
class InputAntiLagVk
{
    // Its input stage with a frame id is the sleep and the simulation start, the present stage the present start
    const static inline InputContext inputContext { .caller = LowLatencyInput::AntiLag2,
                                                    .noFrameId = false,
                                                    .markerMode = InputMarkerMode::SimStartAndPresentStart,
                                                    .api = API::Vulkan };

    // Without the presentation info it's only the sleep, as AntiLag 2 for D3D
    const static inline InputContext noFrameIdContext { .caller = LowLatencyInput::AntiLag2,
                                                        .noFrameId = true,
                                                        .markerMode = InputMarkerMode::SimStartOnly,
                                                        .api = API::Vulkan };

  public:
    static void VKAPI_CALL AntiLagUpdate(VkDevice device, const VkAntiLagDataAMD* pData);
};
