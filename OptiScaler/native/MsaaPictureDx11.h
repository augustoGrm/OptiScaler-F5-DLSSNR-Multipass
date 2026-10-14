#pragma once

// Direct3D only (no OptiScaler headers), so tests/nr_msaa_picture_dx11_gpu.cpp builds it alone.
#include <d3d11.h>
#include <wrl/client.h>

#include <cstdint>

namespace native
{
// Moves a D3D11 game's multisampled swap chain back buffer in and out of a plain single-sample picture. CopyResource
// between textures with different sample counts is dropped by the runtime without a word, so neither direction can be a
// copy.
//
//   ResolveIn: ResolveSubresource averages the samples of each pixel into the picture.
//   WriteBack: a copy cannot write a multisampled texture, a draw can. One triangle over the whole target, a pixel
//   shader that
//              reads the picture at its own pixel; it runs once per pixel and the colour goes to every sample the
//              triangle covers (all of them), so the game's present, which resolves the back buffer, shows the picture
//              exactly.
//
// Both run on the immediate context in the middle of the game's frame. WriteBack puts back everything it touches (input
// layout, topology, the VS, GS, HS, DS and PS with their class instances, PS read view 0, the rasterizer, blend and
// depth-stencil states, the viewports, the render targets, depth view and pixel shader UAVs); ResolveIn touches no
// state. Not thread-safe: the caller guards it. What is kept between calls is made on the device of the resources, and
// made again for another device.
class MsaaPictureDx11
{
  public:
    MsaaPictureDx11() = default;
    MsaaPictureDx11(const MsaaPictureDx11&) = delete;
    MsaaPictureDx11& operator=(const MsaaPictureDx11&) = delete;

    // Resolves `source` (multisampled) into `dest` (one sample, same size). `dest` may be of another format of the same
    // family (the shared picture's format can differ from the back buffer's, e.g. sRGB or typeless): the resolve then
    // goes into a texture of the source's format and is copied from there, which keeps the bits as they are. Returns
    // nullptr when done, otherwise why not (a fixed string, so a caller can log each reason once).
    const char* ResolveIn(ID3D11DeviceContext* context, ID3D11Texture2D* source, ID3D11Texture2D* dest);

    // Draws `source` (one sample, needs a shader resource bind) into every sample of `dest` (multisampled, needs a
    // render target bind, same size). The bits go across as they are: an sRGB back buffer is written through its plain
    // view.
    const char* WriteBack(ID3D11DeviceContext* context, ID3D11Texture2D* source, ID3D11Texture2D* dest);

    // Lets go of everything made, and of the device. Call when the picture is made again or the swap chain is resized:
    // this holds the picture (never the back buffer, whose view lives only inside WriteBack, so a resize is not
    // blocked).
    void Release();

  private:
    bool MakePipeline(ID3D11Device* device);
    const char* MakeStage(ID3D11Device* device, uint32_t width, uint32_t height, DXGI_FORMAT format);

    Microsoft::WRL::ComPtr<ID3D11Device> _device; // what everything below was made on
    Microsoft::WRL::ComPtr<ID3D11VertexShader> _vs;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> _ps;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> _rasterizer;
    Microsoft::WRL::ComPtr<ID3D11BlendState> _blend;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> _depth;
    bool _pipelineFailed = false;

    Microsoft::WRL::ComPtr<ID3D11Texture2D> _stage; // the resolve target when the picture is of another format
    uint32_t _stageWidth = 0, _stageHeight = 0;
    DXGI_FORMAT _stageFormat = DXGI_FORMAT_UNKNOWN;
    bool _stageFailed = false;

    Microsoft::WRL::ComPtr<ID3D11Texture2D> _srvSource; // the picture _srv reads
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> _srv;
};
} // namespace native
