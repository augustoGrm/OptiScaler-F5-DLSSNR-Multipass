#pragma once

#include "menu_dx_base.h"
#include <d3d12.h>

class Menu_Dx12 : public MenuDxBase
{
  private:
    bool _dx12Init = false;
    ID3D12Device* _device = nullptr;
    DXGI_FORMAT _rtvFormat = DXGI_FORMAT_UNKNOWN; // ImGui's pipeline is made for this output format

    // Old resources
    ID3D12Resource* _renderTargetResource[2] = { nullptr, nullptr };
    ID3D12DescriptorHeap* _rtvDescHeap = nullptr;
    ID3D12DescriptorHeap* _srvDescHeap = nullptr;
    D3D12_CPU_DESCRIPTOR_HANDLE _renderTargetDescriptor[2] = {};

    void CreateRenderTarget(const D3D12_RESOURCE_DESC& InDesc);

  public:
    bool Render(ID3D12GraphicsCommandList* pCmdList, ID3D12Resource* outTexture);

    // Can draw for a feature on this device into an output of this format (menu/MenuLifetimeRule.h)
    bool Fits(ID3D12Device* device, DXGI_FORMAT outFormat) const
    {
        return device == _device && (!_dx12Init || outFormat == _rtvFormat);
    }

    Menu_Dx12(HWND handle, ID3D12Device* pDevice);

    ~Menu_Dx12();
};
