#pragma once

#include "SysUtils.h"
#include <shaders/Shader_Vk.h>

// DLSS-NR's dedicated enlarge pass for the "model resolution < 100%" case (Vulkan). Mirrors
// SGSR1_Dx12: Qualcomm's SGSR1 (single-pass, edge-directed 12-tap Lanczos-like upsample +
// adaptive sharpen), used instead of the implicit bilinear tap the resolve pass would otherwise
// get when its answer/proxy texture is smaller than native. One fixed shader -- no filter
// choice, unlike OS_Vk's Scaler switch.
class SGSR1_Vk : public Shader_Vk
{
  public:
    SGSR1_Vk(std::string InName, VkDevice InDevice, VkPhysicalDevice InPhysicalDevice);
    ~SGSR1_Vk() = default;

    // reversibleMode/passthrough/edgeThreshold/edgeSharpness mirror SGSR1_Dx12::Dispatch's own
    // parameters exactly -- see that header's comment for why they're needed (undo/redo the
    // Neutwo/Hybrid domain curve around the edge-directed math). edgeThreshold/edgeSharpness are
    // fixed constants (0.300/2.00) passed in from the caller, not user-configurable.
    bool Dispatch(VkCommandBuffer InCmdList, const VkImageInfo& InResourceView, const VkImageInfo& OutResourceView,
                  uint32_t reversibleMode, uint32_t passthrough, float edgeThreshold, float edgeSharpness);
};
