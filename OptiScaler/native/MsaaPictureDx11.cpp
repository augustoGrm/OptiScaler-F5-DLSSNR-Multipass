// Not built with the precompiled header: Direct3D only, so tests/nr_msaa_picture_dx11_gpu.cpp compiles it alone.
#include "MsaaPictureDx11.h"

#include "F5LowShaderBytecode.h"

#include <cstring>

using Microsoft::WRL::ComPtr;

namespace native
{
namespace
{
// The format a view or a resolve of this texture uses: the plain (not sRGB) typed format of its family, so the bits go across
// as they are.
DXGI_FORMAT PlainFormat(DXGI_FORMAT format)
{
    switch (format)
    {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_B8G8R8X8_TYPELESS:
    case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
        return DXGI_FORMAT_B8G8R8X8_UNORM;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
        return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
        return DXGI_FORMAT_R16G16B16A16_FLOAT;
    default:
        return format;
    }
}

// The format itself when it is typed (an sRGB one stays sRGB), else the plain typed format of its family.
DXGI_FORMAT TypedFormat(DXGI_FORMAT format)
{
    switch (format)
    {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8X8_TYPELESS:
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
        return PlainFormat(format);
    default:
        return format;
    }
}

// A shader stage as the game had it bound, class instances included.
template <typename Shader> struct SavedShader
{
    ComPtr<Shader> shader;
    ID3D11ClassInstance* instances[D3D11_SHADER_MAX_INTERFACES] {};
    UINT instanceCount = D3D11_SHADER_MAX_INTERFACES;

    ~SavedShader()
    {
        for (UINT i = 0; i < instanceCount; ++i)
        {
            if (instances[i] != nullptr)
                instances[i]->Release();
        }
    }
};
} // namespace

void MsaaPictureDx11::Release()
{
    _vs.Reset();
    _ps.Reset();
    _rasterizer.Reset();
    _blend.Reset();
    _depth.Reset();
    _pipelineFailed = false;
    _stage.Reset();
    _stageWidth = _stageHeight = 0;
    _stageFormat = DXGI_FORMAT_UNKNOWN;
    _stageFailed = false;
    _srv.Reset();
    _srvSource.Reset();
    _device.Reset();
}

bool MsaaPictureDx11::MakePipeline(ID3D11Device* device)
{
    if (device != _device.Get())
    {
        Release();
        _device = device;
    }

    if (_pipelineFailed)
        return false;

    if (_vs != nullptr)
        return true;

    // Compiled ahead of time (native/MsaaPicture_Hlsl.h -> native/F5LowShaderBytecode.h): a compile here would run on the
    // game's present thread.
    const auto* vsCode = F5LowShaderBytecode::Find("MsaaPicture", "FullScreenVS");
    const auto* psCode = F5LowShaderBytecode::Find("MsaaPicture", "WriteBackPS");

    D3D11_RASTERIZER_DESC rasterizer {};
    rasterizer.FillMode = D3D11_FILL_SOLID;
    rasterizer.CullMode = D3D11_CULL_NONE;
    rasterizer.DepthClipEnable = TRUE;
    rasterizer.MultisampleEnable = TRUE;

    D3D11_BLEND_DESC blend {};
    blend.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;

    D3D11_DEPTH_STENCIL_DESC depth {};
    depth.DepthEnable = FALSE;
    depth.StencilEnable = FALSE;

    if (vsCode == nullptr || psCode == nullptr ||
        FAILED(device->CreateVertexShader(vsCode->GetBufferPointer(), vsCode->GetBufferSize(), nullptr, &_vs)) ||
        FAILED(device->CreatePixelShader(psCode->GetBufferPointer(), psCode->GetBufferSize(), nullptr, &_ps)) ||
        FAILED(device->CreateRasterizerState(&rasterizer, &_rasterizer)) ||
        FAILED(device->CreateBlendState(&blend, &_blend)) || FAILED(device->CreateDepthStencilState(&depth, &_depth)))
    {
        _vs.Reset();
        _ps.Reset();
        _rasterizer.Reset();
        _blend.Reset();
        _depth.Reset();
        _pipelineFailed = true;
        return false;
    }

    return true;
}

const char* MsaaPictureDx11::MakeStage(ID3D11Device* device, uint32_t width, uint32_t height, DXGI_FORMAT format)
{
    const bool same = _stageWidth == width && _stageHeight == height && _stageFormat == format;

    if (_stage != nullptr && same)
        return nullptr;

    if (_stageFailed && same)
        return "making the resolve texture failed";

    _stage.Reset();
    _stageWidth = width;
    _stageHeight = height;
    _stageFormat = format;
    _stageFailed = false;

    D3D11_TEXTURE2D_DESC desc {};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;

    if (FAILED(device->CreateTexture2D(&desc, nullptr, &_stage)))
    {
        _stage.Reset();
        _stageFailed = true;
        return "making the resolve texture failed";
    }

    return nullptr;
}

const char* MsaaPictureDx11::ResolveIn(ID3D11DeviceContext* context, ID3D11Texture2D* source, ID3D11Texture2D* dest)
{
    if (context->GetType() == D3D11_DEVICE_CONTEXT_DEFERRED)
        return "the context is a deferred one";

    D3D11_TEXTURE2D_DESC sourceDesc {}, destDesc {};
    source->GetDesc(&sourceDesc);
    dest->GetDesc(&destDesc);

    if (sourceDesc.SampleDesc.Count < 2 || destDesc.SampleDesc.Count != 1)
        return "the back buffer is not multisampled, or the picture is";

    if (sourceDesc.Width != destDesc.Width || sourceDesc.Height != destDesc.Height)
        return "the picture is not the size of the back buffer";

    if (PlainFormat(sourceDesc.Format) != PlainFormat(destDesc.Format))
        return "the picture is of another format family than the back buffer";

    // Resolved as the game's present would resolve it: an sRGB back buffer averages in linear light.
    const DXGI_FORMAT resolveFormat = TypedFormat(sourceDesc.Format);

    if (sourceDesc.Format == destDesc.Format)
    {
        context->ResolveSubresource(dest, 0, source, 0, resolveFormat);
        return nullptr;
    }

    // Another format of the family: the resolve takes the source's format and the copy then carries the bits across (a copy
    // between formats of one family is allowed; a resolve into a differently typed texture is not always).
    ComPtr<ID3D11Device> device;
    source->GetDevice(&device);

    if (device == nullptr)
        return "the back buffer has no device";

    if (device != _device)
    {
        Release();
        _device = device;
    }

    if (const char* failed = MakeStage(device.Get(), sourceDesc.Width, sourceDesc.Height, sourceDesc.Format))
        return failed;

    context->ResolveSubresource(_stage.Get(), 0, source, 0, resolveFormat);
    context->CopyResource(dest, _stage.Get());
    return nullptr;
}

const char* MsaaPictureDx11::WriteBack(ID3D11DeviceContext* context, ID3D11Texture2D* source, ID3D11Texture2D* dest)
{
    if (context->GetType() == D3D11_DEVICE_CONTEXT_DEFERRED)
        return "the context is a deferred one";

    D3D11_TEXTURE2D_DESC sourceDesc {}, destDesc {};
    source->GetDesc(&sourceDesc);
    dest->GetDesc(&destDesc);

    if (sourceDesc.SampleDesc.Count != 1 || destDesc.SampleDesc.Count < 2)
        return "the picture is multisampled, or the back buffer is not";

    if (sourceDesc.Width != destDesc.Width || sourceDesc.Height != destDesc.Height)
        return "the picture is not the size of the back buffer";

    if ((sourceDesc.BindFlags & D3D11_BIND_SHADER_RESOURCE) == 0)
        return "the picture cannot be read by a shader";

    if ((destDesc.BindFlags & D3D11_BIND_RENDER_TARGET) == 0)
        return "the back buffer cannot be drawn to";

    ComPtr<ID3D11Device> device;
    dest->GetDevice(&device);

    if (device == nullptr)
        return "the back buffer has no device";

    if (!MakePipeline(device.Get()))
        return "making the write-back pass failed";

    // The picture's read view is kept while the picture stays the same; the back buffer's target view never is (a swap chain
    // cannot be resized while anything still holds one of its buffers).
    if (_srv == nullptr || _srvSource.Get() != source)
    {
        _srv.Reset();
        _srvSource.Reset();

        D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc {};
        srvDesc.Format = PlainFormat(sourceDesc.Format);
        srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MipLevels = 1;

        if (FAILED(device->CreateShaderResourceView(source, &srvDesc, &_srv)))
        {
            _srv.Reset();
            return "making the picture's read view failed";
        }

        _srvSource = source;
    }

    D3D11_RENDER_TARGET_VIEW_DESC rtvDesc {};
    rtvDesc.Format = PlainFormat(destDesc.Format);
    rtvDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DMS;

    ComPtr<ID3D11RenderTargetView> rtv;

    // A swap chain's back buffer may be viewed as the plain format whatever its own (an sRGB one included); the view of the
    // resource's own format is not a fallback, it would encode the picture's bits a second time.
    if (FAILED(device->CreateRenderTargetView(dest, &rtvDesc, &rtv)))
        return "making the back buffer's target view failed";

    // Everything the draw changes, as the game had it.
    ComPtr<ID3D11InputLayout> gameLayout;
    D3D11_PRIMITIVE_TOPOLOGY gameTopology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    SavedShader<ID3D11VertexShader> gameVs;
    SavedShader<ID3D11HullShader> gameHs;
    SavedShader<ID3D11DomainShader> gameDs;
    SavedShader<ID3D11GeometryShader> gameGs;
    SavedShader<ID3D11PixelShader> gamePs;
    ComPtr<ID3D11ShaderResourceView> gameSrv;
    ComPtr<ID3D11RasterizerState> gameRasterizer;
    ComPtr<ID3D11BlendState> gameBlend;
    FLOAT gameBlendFactor[4] {};
    UINT gameSampleMask = 0xffffffff;
    ComPtr<ID3D11DepthStencilState> gameDepthState;
    UINT gameStencilRef = 0;
    UINT viewportCount = 0;
    context->RSGetViewports(&viewportCount, nullptr);
    D3D11_VIEWPORT gameViewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE] {};
    viewportCount = viewportCount < D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE
                        ? viewportCount
                        : D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    context->RSGetViewports(&viewportCount, gameViewports);

    ComPtr<ID3D11RenderTargetView> gameRtvs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT];
    ComPtr<ID3D11DepthStencilView> gameDsv;
    ComPtr<ID3D11UnorderedAccessView> gameUavs[D3D11_PS_CS_UAV_REGISTER_COUNT];
    ID3D11RenderTargetView* rawRtvs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] {};

    context->IAGetInputLayout(&gameLayout);
    context->IAGetPrimitiveTopology(&gameTopology);
    context->VSGetShader(&gameVs.shader, gameVs.instances, &gameVs.instanceCount);
    context->HSGetShader(&gameHs.shader, gameHs.instances, &gameHs.instanceCount);
    context->DSGetShader(&gameDs.shader, gameDs.instances, &gameDs.instanceCount);
    context->GSGetShader(&gameGs.shader, gameGs.instances, &gameGs.instanceCount);
    context->PSGetShader(&gamePs.shader, gamePs.instances, &gamePs.instanceCount);
    context->PSGetShaderResources(0, 1, &gameSrv);
    context->RSGetState(&gameRasterizer);
    context->OMGetBlendState(&gameBlend, gameBlendFactor, &gameSampleMask);
    context->OMGetDepthStencilState(&gameDepthState, &gameStencilRef);

    // Render targets and pixel shader UAVs share their slots; read separately, since each read leaves the other out.
    context->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rawRtvs, &gameDsv);

    for (UINT i = 0; i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i)
        gameRtvs[i].Attach(rawRtvs[i]);

    ID3D11UnorderedAccessView* rawUavs[D3D11_PS_CS_UAV_REGISTER_COUNT] {};
    context->OMGetRenderTargetsAndUnorderedAccessViews(0, nullptr, nullptr, 0, D3D11_PS_CS_UAV_REGISTER_COUNT, rawUavs);

    for (UINT i = 0; i < D3D11_PS_CS_UAV_REGISTER_COUNT; ++i)
        gameUavs[i].Attach(rawUavs[i]);

    D3D11_VIEWPORT viewport {};
    viewport.Width = (FLOAT) destDesc.Width;
    viewport.Height = (FLOAT) destDesc.Height;
    viewport.MaxDepth = 1.0f;

    const FLOAT blendFactor[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
    ID3D11RenderTargetView* const target = rtv.Get();
    ID3D11ShaderResourceView* const read = _srv.Get();

    // The depth view is left out (a game's depth buffer has the back buffer's samples or not, and a mismatch drops the draw);
    // no geometry-side stage but the vertex shader runs.
    context->IASetInputLayout(nullptr);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(_vs.Get(), nullptr, 0);
    context->HSSetShader(nullptr, nullptr, 0);
    context->DSSetShader(nullptr, nullptr, 0);
    context->GSSetShader(nullptr, nullptr, 0);
    context->PSSetShader(_ps.Get(), nullptr, 0);
    context->PSSetShaderResources(0, 1, &read);
    context->RSSetState(_rasterizer.Get());
    context->RSSetViewports(1, &viewport);
    context->OMSetBlendState(_blend.Get(), blendFactor, 0xffffffff);
    context->OMSetDepthStencilState(_depth.Get(), 0);
    context->OMSetRenderTargets(1, &target, nullptr);
    context->Draw(3, 0);

    // Back as the game had them. The render targets and the UAVs share slots: the targets are the leading slots, the UAVs start
    // at the first one bound (a game that binds both has them apart, or the runtime would not have taken them).
    context->PSSetShaderResources(0, 1, gameSrv.GetAddressOf());
    context->OMSetDepthStencilState(gameDepthState.Get(), gameStencilRef);
    context->OMSetBlendState(gameBlend.Get(), gameBlendFactor, gameSampleMask);
    context->RSSetViewports(viewportCount, gameViewports);
    context->RSSetState(gameRasterizer.Get());
    context->PSSetShader(gamePs.shader.Get(), gamePs.instances, gamePs.instanceCount);
    context->GSSetShader(gameGs.shader.Get(), gameGs.instances, gameGs.instanceCount);
    context->DSSetShader(gameDs.shader.Get(), gameDs.instances, gameDs.instanceCount);
    context->HSSetShader(gameHs.shader.Get(), gameHs.instances, gameHs.instanceCount);
    context->VSSetShader(gameVs.shader.Get(), gameVs.instances, gameVs.instanceCount);
    context->IASetPrimitiveTopology(gameTopology);
    context->IASetInputLayout(gameLayout.Get());

    UINT rtvCount = 0, firstUav = D3D11_PS_CS_UAV_REGISTER_COUNT, lastUav = 0;

    for (UINT i = 0; i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i)
    {
        if (gameRtvs[i] != nullptr)
            rtvCount = i + 1;

        rawRtvs[i] = gameRtvs[i].Get();
    }

    for (UINT i = 0; i < D3D11_PS_CS_UAV_REGISTER_COUNT; ++i)
    {
        rawUavs[i] = gameUavs[i].Get();

        if (rawUavs[i] != nullptr)
        {
            firstUav = firstUav < i ? firstUav : i;
            lastUav = i;
        }
    }

    if (firstUav > lastUav)
    {
        context->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rawRtvs, gameDsv.Get());
    }
    else
    {
        // -1 keeps an append or counter UAV's hidden count as it is.
        const UINT keepCounts[D3D11_PS_CS_UAV_REGISTER_COUNT] = { (UINT) -1, (UINT) -1, (UINT) -1, (UINT) -1,
                                                                    (UINT) -1, (UINT) -1, (UINT) -1, (UINT) -1 };
        context->OMSetRenderTargetsAndUnorderedAccessViews(rtvCount < firstUav ? rtvCount : firstUav, rawRtvs,
                                                           gameDsv.Get(), firstUav, lastUav - firstUav + 1,
                                                           rawUavs + firstUav, keepCounts);
    }

    return nullptr;
}
} // namespace native
