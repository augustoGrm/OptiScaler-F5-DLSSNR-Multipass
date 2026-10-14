#include "pch.h"
#include "SGSR1_Vk.h"

#include "precompile/sgsr1_Shader_Vk.h"

// Mirrors SGSR1_Dx12.cpp's Sgsr1Constants field-for-field. alignas(256) matches OS_Common.h's
// own Constants struct -- Vulkan UBOs don't strictly require D3D12's 256-byte CBV alignment, but
// this project's Vulkan constant structs already follow it as a house convention (see OS_Common.h).
struct alignas(256) Sgsr1Constants
{
    // x = 1/srcWidth, y = 1/srcHeight, z = srcWidth, w = srcHeight.
    float ViewportInfoX;
    float ViewportInfoY;
    float ViewportInfoZ;
    float ViewportInfoW;
    int32_t DstWidth;
    int32_t DstHeight;
    uint32_t ReversibleMode;
    uint32_t Passthrough;
    // Live-tunable versions of what used to be sgsr1.hlsl's own kEdgeThreshold/kEdgeSharpness
    // constants (8/255, 2.0) -- see SGSR1_Vk.h's Dispatch() comment for why.
    float EdgeThreshold;
    float EdgeSharpness;
};

static Sgsr1Constants constants {};

SGSR1_Vk::SGSR1_Vk(std::string InName, VkDevice InDevice, VkPhysicalDevice InPhysicalDevice)
    : Shader_Vk(InName, InDevice, InPhysicalDevice)
{
    if (InDevice == VK_NULL_HANDLE)
    {
        LOG_ERROR("InDevice is nullptr!");
        return;
    }

    LOG_FUNC();

    CreateSampler(VK_FILTER_LINEAR, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE);
    CreateConstantBuffer(sizeof(Sgsr1Constants));

    // Binding order matches sgsr1.hlsl's VK_MODE attributes exactly: UBO 0, sampled image 1,
    // storage image 2, sampler 3 -- same order OS_Vk uses for its own 4-binding layout.
    std::vector<VkDescriptorSetLayoutBinding> bindings = { CreateBinding(0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER),
                                                           CreateBinding(1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE),
                                                           CreateBinding(2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE),
                                                           CreateBinding(3, VK_DESCRIPTOR_TYPE_SAMPLER) };
    CreateLayouts(bindings);

    std::vector<VkDescriptorPoolSize> poolSizes = { { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, _maxFramesInFlight },
                                                    { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, _maxFramesInFlight },
                                                    { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, _maxFramesInFlight },
                                                    { VK_DESCRIPTOR_TYPE_SAMPLER, _maxFramesInFlight } };

    CreateDescriptorPool(poolSizes, _maxFramesInFlight);
    CreateDescriptorSets(_descriptorSetLayout, _descriptorPool, _descriptorSets);

    // Precompiled only, matching SGSR1_Dx12's own precedent -- no runtime-recompile fallback
    // source, since SGSR1 is not exposed as a user-selectable filter (nothing prompts a
    // recompile toggle to reach it).
    std::vector<char> shaderCode(sgsr1_spv, sgsr1_spv + sizeof(sgsr1_spv));

    if (!CreateComputePipeline(_device, _pipelineLayout, &_pipeline, shaderCode))
    {
        LOG_ERROR("Failed to create pipeline for SGSR1_Vk");
        _init = false;
        return;
    }

    _init = true;
}

bool SGSR1_Vk::Dispatch(VkCommandBuffer InCmdList, const VkImageInfo& InResourceView,
                        const VkImageInfo& OutResourceView, uint32_t reversibleMode, uint32_t passthrough,
                        float edgeThreshold, float edgeSharpness)
{
    if (!_init || InCmdList == VK_NULL_HANDLE)
        return false;

    const uint32_t srcW = InResourceView.Width;
    const uint32_t srcH = InResourceView.Height;
    const uint32_t dstW = OutResourceView.Width;
    const uint32_t dstH = OutResourceView.Height;

    constants.ViewportInfoX = 1.0f / (float) srcW;
    constants.ViewportInfoY = 1.0f / (float) srcH;
    constants.ViewportInfoZ = (float) srcW;
    constants.ViewportInfoW = (float) srcH;
    constants.DstWidth = (int32_t) dstW;
    constants.DstHeight = (int32_t) dstH;
    constants.ReversibleMode = reversibleMode;
    constants.Passthrough = passthrough;
    constants.EdgeThreshold = edgeThreshold;
    constants.EdgeSharpness = edgeSharpness;

    if (_mappedConstantBuffer)
        memcpy(_mappedConstantBuffer, &constants, sizeof(Sgsr1Constants));

    // Advance Frame Index
    _currentSetIndex = (_currentSetIndex + 1) % _maxFramesInFlight;
    VkDescriptorSet currentSet = _descriptorSets[_currentSetIndex];

    // Build Descriptor Writes
    VkDescriptorBufferInfo bufferInfo { _constantBuffer, 0, sizeof(Sgsr1Constants) };
    VkDescriptorImageInfo sourceInfo { VK_NULL_HANDLE, InResourceView.ImageView,
                                       VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    VkDescriptorImageInfo destInfo { VK_NULL_HANDLE, OutResourceView.ImageView, VK_IMAGE_LAYOUT_GENERAL };
    VkDescriptorImageInfo samplerInfo { _textureSampler, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED };

    std::vector<VkWriteDescriptorSet> writes = { { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, currentSet, 0, 0, 1,
                                                   VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, nullptr, &bufferInfo, nullptr },
                                                 { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, currentSet, 1, 0, 1,
                                                   VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, &sourceInfo, nullptr, nullptr },
                                                 { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, currentSet, 2, 0, 1,
                                                   VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &destInfo, nullptr, nullptr },
                                                 { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, currentSet, 3, 0, 1,
                                                   VK_DESCRIPTOR_TYPE_SAMPLER, &samplerInfo, nullptr, nullptr } };

    vkUpdateDescriptorSets(_device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);

    vkCmdBindPipeline(InCmdList, VK_PIPELINE_BIND_POINT_COMPUTE, _pipeline);
    vkCmdBindDescriptorSets(InCmdList, VK_PIPELINE_BIND_POINT_COMPUTE, _pipelineLayout, 0, 1, &currentSet, 0, nullptr);

    // sgsr1.hlsl is [numthreads(8, 8, 1)], matching SGSR1_Dx12's fixed 8x8 dispatch group size.
    const uint32_t groupX = (dstW + 7) / 8;
    const uint32_t groupY = (dstH + 7) / 8;
    vkCmdDispatch(InCmdList, groupX, groupY, 1);

    return true;
}
