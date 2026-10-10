// GPU test for native/MsaaPictureDx11.cpp, a D3D11 game's multisampled back buffer moved in and out of a single-sample picture, no
// game:
//   - ResolveIn of a known sample pattern (one sample white, the rest black) gives the average, for B8G8R8A8, R8G8B8A8, R8G8B8A8
//     sRGB, R10G10B10A2 and R16G16B16A16 float at 2 and 4 samples; into a picture of another format of the family (sRGB back
//     buffer into a plain picture, plain into a typeless one) the bits are the ones the resolve made,
//   - WriteBack of a known picture leaves every sample of every pixel exactly the picture's value (a compute shader compares each
//     sample with the picture, bit for bit), also into an sRGB or typeless back buffer from a plain picture,
//   - every piece of state WriteBack touches, set to something other than the default before, is what it was after (the input
//     layout, topology, the vertex, hull, domain, geometry and pixel shaders, pixel shader read view 0, the rasterizer, blend and
//     depth-stencil states, the viewports, render targets, depth view and a pixel shader UAV), and what it does not touch too,
//   - a single-sample "back buffer", a mismatched size and a deferred context are refused with a reason and change nothing,
//   - with the debug layer installed, the runtime reports no error or warning for any of it.
//
//   vcvars64, then from the repo root:
//   cl /std:c++20 /EHsc /O2 tests\nr_msaa_picture_dx11_gpu.cpp OptiScaler\native\MsaaPictureDx11.cpp d3d11.lib d3dcompiler.lib dxgi.lib user32.lib

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi.h>
#include <wrl/client.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "../OptiScaler/native/F5LowShaderBytecode.h"
#include "../OptiScaler/native/MsaaPictureDx11.h"

using Microsoft::WRL::ComPtr;

namespace
{
int g_failures = 0;

void Check(bool ok, const char* what)
{
    std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
    g_failures += ok ? 0 : 1;
}

struct Device
{
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11InfoQueue> info; // only with the debug layer installed
};

bool MakeDevice(Device* out)
{
    const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;

    if (SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_DEBUG, &level, 1,
                                    D3D11_SDK_VERSION, &out->device, nullptr, &out->context)))
    {
        // Errors, warnings and corruption only: the debug layer also stores plain information messages.
        if (SUCCEEDED(out->device.As(&out->info)))
        {
            D3D11_MESSAGE_SEVERITY deny[] = { D3D11_MESSAGE_SEVERITY_INFO, D3D11_MESSAGE_SEVERITY_MESSAGE };
            D3D11_INFO_QUEUE_FILTER filter {};
            filter.DenyList.NumSeverities = 2;
            filter.DenyList.pSeverityList = deny;
            out->info->PushStorageFilter(&filter);
        }

        return true;
    }

    return SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &level, 1, D3D11_SDK_VERSION,
                                       &out->device, nullptr, &out->context));
}

// Errors and warnings the debug layer has stored since the last call (0 without the debug layer), printed.
UINT64 DebugMessages(Device& d)
{
    if (d.info == nullptr)
        return 0;

    const UINT64 count = d.info->GetNumStoredMessages();

    for (UINT64 i = 0; i < count; ++i)
    {
        SIZE_T size = 0;
        d.info->GetMessage(i, nullptr, &size);
        auto* message = (D3D11_MESSAGE*) std::malloc(size);

        if (message != nullptr && SUCCEEDED(d.info->GetMessage(i, message, &size)))
            std::printf("      debug layer: %.*s\n", (int) message->DescriptionByteLength, message->pDescription);

        std::free(message);
    }

    d.info->ClearStoredMessages();
    return count;
}

ComPtr<ID3DBlob> Compile(const char* source, const char* entry, const char* profile)
{
    ComPtr<ID3DBlob> code, messages;

    if (FAILED(D3DCompile(source, std::strlen(source), "test", nullptr, nullptr, entry, profile, 0, 0, &code, &messages)))
    {
        std::printf("      compiling %s: %s\n", entry,
                    messages != nullptr ? (const char*) messages->GetBufferPointer() : "no message");
        return nullptr;
    }

    return code;
}

enum class Kind { U8, R10, F16 };

struct FormatCase
{
    const char* name;
    DXGI_FORMAT format;
    DXGI_FORMAT plain; // the format the bits are read as
    Kind kind;
    bool srgb;
    UINT bytesPerPixel;
};

const FormatCase kFormats[] = {
    { "B8G8R8A8_UNORM", DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM, Kind::U8, false, 4 },
    { "R8G8B8A8_UNORM", DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM, Kind::U8, false, 4 },
    { "R8G8B8A8_UNORM_SRGB", DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, DXGI_FORMAT_R8G8B8A8_UNORM, Kind::U8, true, 4 },
    { "R10G10B10A2_UNORM", DXGI_FORMAT_R10G10B10A2_UNORM, DXGI_FORMAT_R10G10B10A2_UNORM, Kind::R10, false, 4 },
    { "R16G16B16A16_FLOAT", DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R16G16B16A16_FLOAT, Kind::F16, false, 8 },
};

constexpr UINT kWidth = 67, kHeight = 35; // odd on purpose: pitch and the compute groups' edges

float HalfToFloat(uint16_t h)
{
    const uint32_t sign = (h >> 15) & 1, exponent = (h >> 10) & 31, mantissa = h & 1023;
    float value;

    if (exponent == 0)
        value = std::ldexp((float) mantissa, -24);
    else if (exponent == 31)
        value = INFINITY;
    else
        value = std::ldexp((float) (mantissa | 1024), (int) exponent - 25);

    return sign != 0 ? -value : value;
}

float SrgbEncode(float linear)
{
    return linear <= 0.0031308f ? linear * 12.92f : 1.055f * std::pow(linear, 1.0f / 2.4f) - 0.055f;
}

// A texel's first three channels as they are stored (0..1, not sRGB-decoded), in storage order.
void Decode(const FormatCase& f, const uint8_t* texel, float rgb[3])
{
    switch (f.kind)
    {
    case Kind::U8:
        for (int i = 0; i < 3; ++i)
            rgb[i] = texel[i] / 255.0f;
        break;
    case Kind::R10:
    {
        uint32_t packed;
        std::memcpy(&packed, texel, 4);
        rgb[0] = (packed & 1023) / 1023.0f;
        rgb[1] = ((packed >> 10) & 1023) / 1023.0f;
        rgb[2] = ((packed >> 20) & 1023) / 1023.0f;
        break;
    }
    case Kind::F16:
        for (int i = 0; i < 3; ++i)
        {
            uint16_t h;
            std::memcpy(&h, texel + i * 2, 2);
            rgb[i] = HalfToFloat(h);
        }
        break;
    }
}

bool Supports(Device& d, DXGI_FORMAT format, UINT samples)
{
    UINT support = 0;
    UINT levels = 0;

    if (FAILED(d.device->CheckFormatSupport(format, &support)) ||
        FAILED(d.device->CheckMultisampleQualityLevels(format, samples, &levels)) || levels == 0)
        return false;

    return (support & D3D11_FORMAT_SUPPORT_MULTISAMPLE_RENDERTARGET) != 0 &&
           (support & D3D11_FORMAT_SUPPORT_MULTISAMPLE_RESOLVE) != 0;
}

ComPtr<ID3D11Texture2D> MakeTexture(Device& d, DXGI_FORMAT format, UINT samples, UINT bind, UINT width = kWidth,
                                    UINT height = kHeight)
{
    D3D11_TEXTURE2D_DESC desc {};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = format;
    desc.SampleDesc.Count = samples;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = bind;

    ComPtr<ID3D11Texture2D> tex;

    if (FAILED(d.device->CreateTexture2D(&desc, nullptr, &tex)))
        return nullptr;

    return tex;
}

// A multisampled swap chain's back buffer (blt model, as the games that use MSAA have it). A game's back buffer is the one kind
// of sRGB texture that may be viewed as the plain format, which is how the write-back draws to it.
struct BackBuffer
{
    HWND window = nullptr;
    ComPtr<IDXGISwapChain> chain;
    ComPtr<ID3D11Texture2D> tex;

    BackBuffer() = default;
    BackBuffer(const BackBuffer&) = delete;
    BackBuffer& operator=(const BackBuffer&) = delete;

    ~BackBuffer()
    {
        tex.Reset();
        chain.Reset();

        if (window != nullptr)
            DestroyWindow(window);
    }

    bool Make(Device& d, DXGI_FORMAT format, UINT samples)
    {
        window = CreateWindowA("STATIC", "msaa test", WS_OVERLAPPED, 0, 0, 64, 64, nullptr, nullptr, nullptr, nullptr);

        ComPtr<IDXGIDevice> dxgiDevice;
        ComPtr<IDXGIAdapter> adapter;
        ComPtr<IDXGIFactory> factory;

        if (window == nullptr || FAILED(d.device.As(&dxgiDevice)) || FAILED(dxgiDevice->GetAdapter(&adapter)) ||
            FAILED(adapter->GetParent(IID_PPV_ARGS(&factory))))
            return false;

        DXGI_SWAP_CHAIN_DESC desc {};
        desc.BufferDesc.Width = kWidth;
        desc.BufferDesc.Height = kHeight;
        desc.BufferDesc.Format = format;
        desc.SampleDesc.Count = samples;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT | DXGI_USAGE_SHADER_INPUT;
        desc.BufferCount = 1;
        desc.OutputWindow = window;
        desc.Windowed = TRUE;
        desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

        return SUCCEEDED(factory->CreateSwapChain(d.device.Get(), &desc, &chain)) &&
               SUCCEEDED(chain->GetBuffer(0, IID_PPV_ARGS(&tex)));
    }
};

// The texture's bytes, row by row without the pitch.
bool ReadBack(Device& d, ID3D11Texture2D* tex, UINT bytesPerPixel, std::vector<uint8_t>* out)
{
    D3D11_TEXTURE2D_DESC desc {};
    tex->GetDesc(&desc);
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

    ComPtr<ID3D11Texture2D> staging;

    if (FAILED(d.device->CreateTexture2D(&desc, nullptr, &staging)))
        return false;

    d.context->CopyResource(staging.Get(), tex);

    D3D11_MAPPED_SUBRESOURCE mapped {};

    if (FAILED(d.context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped)))
        return false;

    out->resize((size_t) desc.Width * desc.Height * bytesPerPixel);

    for (UINT y = 0; y < desc.Height; ++y)
        std::memcpy(out->data() + (size_t) y * desc.Width * bytesPerPixel, (const uint8_t*) mapped.pData + y * mapped.RowPitch,
                    (size_t) desc.Width * bytesPerPixel);

    d.context->Unmap(staging.Get(), 0);
    return true;
}

// The pipeline the test draws its own multisampled content with.
struct Painter
{
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> oneSample; // white into sample 0 only
    ComPtr<ID3D11RasterizerState> rasterizer;
    ComPtr<ID3D11ComputeShader> compare;

    bool Make(Device& d)
    {
        const auto* vsCode = F5LowShaderBytecode::Find("MsaaPicture", "FullScreenVS");
        const auto ps = Compile("struct O { float4 c : SV_Target; uint m : SV_Coverage; };\n"
                                "O main(float4 p : SV_Position) { O o; o.c = float4(1, 1, 1, 1); o.m = 1; return o; }",
                                "main", "ps_5_0");
        const auto cs = Compile("Texture2DMS<float4> Ms : register(t0);\n"
                                "Texture2D<float4> Pic : register(t1);\n"
                                "RWByteAddressBuffer Bad : register(u0);\n"
                                "[numthreads(8, 8, 1)] void main(uint3 id : SV_DispatchThreadID) {\n"
                                "  uint w, h, n; Ms.GetDimensions(w, h, n);\n"
                                "  if (id.x >= w || id.y >= h) return;\n"
                                "  const uint4 p = asuint(Pic.Load(int3(id.xy, 0)));\n"
                                "  for (uint s = 0; s < n; ++s) {\n"
                                "    if (any(asuint(Ms.Load(int2(id.xy), s)) != p)) { uint o; Bad.InterlockedAdd(0, 1, o); }\n"
                                "  }\n}",
                                "main", "cs_5_0");

        D3D11_RASTERIZER_DESC rs {};
        rs.FillMode = D3D11_FILL_SOLID;
        rs.CullMode = D3D11_CULL_NONE;
        rs.DepthClipEnable = TRUE;
        rs.MultisampleEnable = TRUE;

        return vsCode != nullptr && ps != nullptr && cs != nullptr &&
               SUCCEEDED(d.device->CreateVertexShader(vsCode->GetBufferPointer(), vsCode->GetBufferSize(), nullptr, &vs)) &&
               SUCCEEDED(d.device->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &oneSample)) &&
               SUCCEEDED(d.device->CreateComputeShader(cs->GetBufferPointer(), cs->GetBufferSize(), nullptr, &compare)) &&
               SUCCEEDED(d.device->CreateRasterizerState(&rs, &rasterizer));
    }

    // Black everywhere, then white in sample 0 only: a resolve of n samples is 1/n.
    bool FillOneSample(Device& d, ID3D11Texture2D* msaa)
    {
        D3D11_TEXTURE2D_DESC desc {};
        msaa->GetDesc(&desc);

        D3D11_RENDER_TARGET_VIEW_DESC rtvDesc {};
        rtvDesc.Format = desc.Format;
        rtvDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DMS;

        ComPtr<ID3D11RenderTargetView> rtv;

        if (FAILED(d.device->CreateRenderTargetView(msaa, &rtvDesc, &rtv)))
            return false;

        const FLOAT black[4] = {};
        D3D11_VIEWPORT viewport { 0, 0, (FLOAT) desc.Width, (FLOAT) desc.Height, 0, 1 };
        ID3D11RenderTargetView* target = rtv.Get();

        d.context->ClearRenderTargetView(rtv.Get(), black);
        d.context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        d.context->VSSetShader(vs.Get(), nullptr, 0);
        d.context->PSSetShader(oneSample.Get(), nullptr, 0);
        d.context->RSSetState(rasterizer.Get());
        d.context->RSSetViewports(1, &viewport);
        d.context->OMSetRenderTargets(1, &target, nullptr);
        d.context->Draw(3, 0);
        d.context->ClearState();
        return true;
    }

    // How many samples of `msaa` differ from `picture` at their pixel, bit for bit (-1 when the check could not run).
    int Mismatches(Device& d, ID3D11Texture2D* msaa, ID3D11Texture2D* picture, DXGI_FORMAT plain)
    {
        D3D11_TEXTURE2D_DESC desc {};
        msaa->GetDesc(&desc);

        D3D11_SHADER_RESOURCE_VIEW_DESC msDesc {};
        msDesc.Format = plain;
        msDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DMS;

        D3D11_SHADER_RESOURCE_VIEW_DESC picDesc {};
        picDesc.Format = plain;
        picDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        picDesc.Texture2D.MipLevels = 1;

        D3D11_BUFFER_DESC bufDesc {};
        bufDesc.ByteWidth = 16;
        bufDesc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
        bufDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;

        D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc {};
        uavDesc.Format = DXGI_FORMAT_R32_TYPELESS;
        uavDesc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
        uavDesc.Buffer.NumElements = 4;
        uavDesc.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;

        ComPtr<ID3D11ShaderResourceView> msSrv, picSrv;
        ComPtr<ID3D11Buffer> buffer, staging;
        ComPtr<ID3D11UnorderedAccessView> uav;
        const UINT zero[4] = {};

        if (FAILED(d.device->CreateShaderResourceView(msaa, &msDesc, &msSrv)) ||
            FAILED(d.device->CreateShaderResourceView(picture, &picDesc, &picSrv)) ||
            FAILED(d.device->CreateBuffer(&bufDesc, nullptr, &buffer)) ||
            FAILED(d.device->CreateUnorderedAccessView(buffer.Get(), &uavDesc, &uav)))
            return -1;

        bufDesc.BindFlags = 0;
        bufDesc.MiscFlags = 0;
        bufDesc.Usage = D3D11_USAGE_STAGING;
        bufDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

        if (FAILED(d.device->CreateBuffer(&bufDesc, nullptr, &staging)))
            return -1;

        ID3D11ShaderResourceView* srvs[2] = { msSrv.Get(), picSrv.Get() };
        ID3D11UnorderedAccessView* uavs[1] = { uav.Get() };
        d.context->ClearUnorderedAccessViewUint(uav.Get(), zero);
        d.context->CSSetShader(compare.Get(), nullptr, 0);
        d.context->CSSetShaderResources(0, 2, srvs);
        d.context->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);
        d.context->Dispatch((desc.Width + 7) / 8, (desc.Height + 7) / 8, 1);
        d.context->ClearState();
        d.context->CopyResource(staging.Get(), buffer.Get());

        D3D11_MAPPED_SUBRESOURCE mapped {};

        if (FAILED(d.context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped)))
            return -1;

        const int bad = *(const int*) mapped.pData;
        d.context->Unmap(staging.Get(), 0);
        return bad;
    }
};

// ResolveIn of a one-white-sample pattern: every pixel is 1/samples (sRGB: that, encoded), as the destination stores it.
void ResolveCases(Device& d, Painter& painter, native::MsaaPictureDx11& pic)
{
    for (const FormatCase& f : kFormats)
    {
        for (UINT samples : { 2u, 4u })
        {
            char label[128];

            if (!Supports(d, f.format, samples))
            {
                std::printf("skip  %s x%u not supported by this adapter\n", f.name, samples);
                continue;
            }

            auto msaa = MakeTexture(d, f.format, samples, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);
            auto dest = MakeTexture(d, f.format, 1, D3D11_BIND_SHADER_RESOURCE);
            std::snprintf(label, sizeof(label), "%s x%u: made the textures and painted the pattern", f.name, samples);
            Check(msaa != nullptr && dest != nullptr && painter.FillOneSample(d, msaa.Get()), label);

            if (msaa == nullptr || dest == nullptr)
                continue;

            const char* why = pic.ResolveIn(d.context.Get(), msaa.Get(), dest.Get());
            std::vector<uint8_t> bytes;
            const bool read = ReadBack(d, dest.Get(), f.bytesPerPixel, &bytes);
            const float linear = 1.0f / samples;
            const float expected = f.srgb ? SrgbEncode(linear) : linear;
            const float tolerance = f.kind == Kind::F16 ? 0.002f : 2.5f / (f.kind == Kind::R10 ? 1023.0f : 255.0f);
            bool allRight = read && why == nullptr;
            float worst = 0;

            for (size_t i = 0; read && i < (size_t) kWidth * kHeight; ++i)
            {
                float rgb[3];
                Decode(f, bytes.data() + i * f.bytesPerPixel, rgb);

                for (int c = 0; c < 3; ++c)
                {
                    worst = std::fmax(worst, std::fabs(rgb[c] - expected));
                    allRight = allRight && std::fabs(rgb[c] - expected) <= tolerance;
                }
            }

            std::snprintf(label, sizeof(label), "%s x%u: ResolveIn gives the average (%.4f, worst off by %.4f)", f.name,
                          samples, expected, worst);
            Check(allRight, label);

            if (why != nullptr)
                std::printf("      reason: %s\n", why);
        }
    }

    // Another format of the family: an sRGB back buffer into a plain picture, a plain one into a typeless picture. The bits are
    // the resolve's, just as a copy between the two would carry them.
    struct Pair
    {
        const char* name;
        DXGI_FORMAT source, dest;
        const FormatCase* as;
    };
    const Pair pairs[] = {
        { "R8G8B8A8_UNORM_SRGB -> R8G8B8A8_UNORM", DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, DXGI_FORMAT_R8G8B8A8_UNORM, &kFormats[2] },
        { "B8G8R8A8_UNORM -> B8G8R8A8_TYPELESS", DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_B8G8R8A8_TYPELESS, &kFormats[0] },
        { "R8G8B8A8_UNORM -> R8G8B8A8_TYPELESS", DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_TYPELESS, &kFormats[1] },
    };

    for (const Pair& p : pairs)
    {
        auto msaa = MakeTexture(d, p.source, 4, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);
        auto dest = MakeTexture(d, p.dest, 1, 0);
        const bool made = msaa != nullptr && dest != nullptr && painter.FillOneSample(d, msaa.Get());
        char label[160];
        std::snprintf(label, sizeof(label), "%s: made the textures and painted the pattern", p.name);
        Check(made, label);

        if (!made)
            continue;

        const char* why = pic.ResolveIn(d.context.Get(), msaa.Get(), dest.Get());
        std::vector<uint8_t> bytes;
        const bool read = ReadBack(d, dest.Get(), 4, &bytes);
        const float expected = p.as->srgb ? SrgbEncode(0.25f) : 0.25f;
        bool allRight = read && why == nullptr;

        for (size_t i = 0; read && i < (size_t) kWidth * kHeight; ++i)
        {
            float rgb[3];
            Decode(*p.as, bytes.data() + i * 4, rgb);

            for (int c = 0; c < 3; ++c)
                allRight = allRight && std::fabs(rgb[c] - expected) <= 2.5f / 255.0f;
        }

        std::snprintf(label, sizeof(label), "%s: ResolveIn into the other format keeps the resolve's bits", p.name);
        Check(allRight, label);

        if (why != nullptr)
            std::printf("      reason: %s\n", why);
    }
}

// A picture of known, varied bits of format `f` (valid values only: finite halves).
std::vector<uint8_t> PictureBits(const FormatCase& f, int seed)
{
    std::vector<uint8_t> bits((size_t) kWidth * kHeight * f.bytesPerPixel);

    for (UINT y = 0; y < kHeight; ++y)
    {
        for (UINT x = 0; x < kWidth; ++x)
        {
            uint8_t* texel = bits.data() + ((size_t) y * kWidth + x) * f.bytesPerPixel;

            if (f.kind == Kind::F16)
            {
                for (int c = 0; c < 4; ++c)
                {
                    const uint16_t h = (uint16_t) (0x3000 + (x * 5 + y * 3 + c * 11 + seed) % 0x0800);
                    std::memcpy(texel + c * 2, &h, 2);
                }
            }
            else
            {
                for (UINT c = 0; c < f.bytesPerPixel; ++c)
                    texel[c] = (uint8_t) (x * 7 + y * 13 + c * 31 + seed);
            }
        }
    }

    return bits;
}

ComPtr<ID3D11Texture2D> MakePicture(Device& d, DXGI_FORMAT format, const std::vector<uint8_t>& bits, UINT bytesPerPixel)
{
    D3D11_TEXTURE2D_DESC desc {};
    desc.Width = kWidth;
    desc.Height = kHeight;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;

    D3D11_SUBRESOURCE_DATA data { bits.data(), kWidth * bytesPerPixel, 0 };
    ComPtr<ID3D11Texture2D> tex;

    if (FAILED(d.device->CreateTexture2D(&desc, &data, &tex)))
        return nullptr;

    return tex;
}

void WriteBackCases(Device& d, Painter& painter, native::MsaaPictureDx11& pic)
{
    struct Case
    {
        const FormatCase* f;
        DXGI_FORMAT backBuffer; // the multisampled texture's format
        DXGI_FORMAT picture;
        const char* note;
        DXGI_FORMAT view; // what the compare reads both textures as (a view of an sRGB texture must be sRGB)
        bool roundTrip;   // compared through a resolve instead: the picture's format cannot be viewed as the back buffer's
    };
    std::vector<Case> cases;

    for (const FormatCase& f : kFormats)
        cases.push_back({ &f, f.format, f.srgb ? DXGI_FORMAT_R8G8B8A8_TYPELESS : f.plain,
                          f.srgb ? " (typeless picture)" : "", f.format, false });

    cases.push_back({ &kFormats[2], DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, DXGI_FORMAT_R8G8B8A8_UNORM,
                      " (plain picture, sRGB back buffer)", DXGI_FORMAT_R8G8B8A8_UNORM, true });
    cases.push_back({ &kFormats[0], DXGI_FORMAT_B8G8R8A8_TYPELESS, DXGI_FORMAT_B8G8R8A8_UNORM,
                      " (plain picture, typeless back buffer)", DXGI_FORMAT_B8G8R8A8_UNORM, false });

    for (const Case& c : cases)
    {
        for (UINT samples : { 2u, 4u })
        {
            char label[160];
            const DXGI_FORMAT probe = c.backBuffer == DXGI_FORMAT_B8G8R8A8_TYPELESS ? DXGI_FORMAT_B8G8R8A8_UNORM : c.backBuffer;

            if (!Supports(d, probe, samples))
            {
                std::printf("skip  %s x%u not supported by this adapter\n", c.f->name, samples);
                continue;
            }

            const auto bits = PictureBits(*c.f, (int) samples);
            auto picture = MakePicture(d, c.picture, bits, c.f->bytesPerPixel);
            // A typeless format cannot be a swap chain's; its stand-in is a plain texture.
            BackBuffer chain;
            ComPtr<ID3D11Texture2D> backBuffer;

            if (c.backBuffer == DXGI_FORMAT_B8G8R8A8_TYPELESS)
                backBuffer = MakeTexture(d, c.backBuffer, samples, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);
            else if (chain.Make(d, c.backBuffer, samples))
                backBuffer = chain.tex;

            std::snprintf(label, sizeof(label), "%s x%u%s: made the textures", c.f->name, samples, c.note);
            Check(picture != nullptr && backBuffer != nullptr, label);

            if (picture == nullptr || backBuffer == nullptr)
                continue;

            const char* why = pic.WriteBack(d.context.Get(), picture.Get(), backBuffer.Get());
            d.context->Flush();
            int bad = 0;

            if (!c.roundTrip)
            {
                bad = painter.Mismatches(d, backBuffer.Get(), picture.Get(), c.view);
            }
            else
            {
                // What the game's present shows: the resolve of the drawn back buffer is the picture (within the rounding of the
                // sRGB average of equal samples).
                auto again = MakeTexture(d, c.picture, 1, D3D11_BIND_SHADER_RESOURCE);
                std::vector<uint8_t> out;
                bad = -1;

                if (again != nullptr && pic.ResolveIn(d.context.Get(), backBuffer.Get(), again.Get()) == nullptr &&
                    ReadBack(d, again.Get(), c.f->bytesPerPixel, &out))
                {
                    bad = 0;

                    for (size_t i = 0; i < out.size(); ++i)
                        bad += std::abs((int) out[i] - (int) bits[i]) > 1 ? 1 : 0;
                }
            }

            std::snprintf(label, sizeof(label), "%s x%u%s: WriteBack leaves every sample the picture's value (%d differ)",
                          c.f->name, samples, c.note, bad);
            Check(why == nullptr && bad == 0, label);

            if (why != nullptr)
                std::printf("      reason: %s\n", why);
        }
    }

    // A resolve of the drawn back buffer shows the picture (what the game's present does): the ResolveIn of what WriteBack wrote
    // is the picture again, for a format whose average of equal values is exact.
    {
        const FormatCase& f = kFormats[0];
        const auto bits = PictureBits(f, 3);
        auto picture = MakePicture(d, f.format, bits, f.bytesPerPixel);
        auto backBuffer = MakeTexture(d, f.format, 4, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);
        auto again = MakeTexture(d, f.format, 1, D3D11_BIND_SHADER_RESOURCE);
        std::vector<uint8_t> out;
        const bool ok = picture != nullptr && backBuffer != nullptr && again != nullptr &&
                        pic.WriteBack(d.context.Get(), picture.Get(), backBuffer.Get()) == nullptr &&
                        pic.ResolveIn(d.context.Get(), backBuffer.Get(), again.Get()) == nullptr &&
                        ReadBack(d, again.Get(), f.bytesPerPixel, &out);
        size_t off = 0;

        for (size_t i = 0; ok && i < out.size(); ++i)
            off += std::abs((int) out[i] - (int) bits[i]) > 1 ? 1 : 0;

        Check(ok && off == 0, "B8G8R8A8 x4: WriteBack then ResolveIn returns the picture");
    }
}

// What the context has bound, for the pieces WriteBack changes and a few it does not.
struct Bound
{
    ComPtr<ID3D11InputLayout> layout;
    D3D11_PRIMITIVE_TOPOLOGY topology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    ComPtr<ID3D11Buffer> vertexBuffer, indexBuffer;
    UINT vbStride = 0, vbOffset = 0, ibOffset = 0;
    DXGI_FORMAT ibFormat = DXGI_FORMAT_UNKNOWN;
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11HullShader> hs;
    ComPtr<ID3D11DomainShader> ds;
    ComPtr<ID3D11GeometryShader> gs;
    ComPtr<ID3D11PixelShader> ps;
    ComPtr<ID3D11ShaderResourceView> srv;
    ComPtr<ID3D11RasterizerState> rasterizer;
    ComPtr<ID3D11BlendState> blend;
    FLOAT blendFactor[4] {};
    UINT sampleMask = 0;
    ComPtr<ID3D11DepthStencilState> depthState;
    UINT stencilRef = 0;
    UINT viewportCount = 0;
    D3D11_VIEWPORT viewports[4] {};
    UINT scissorCount = 0;
    D3D11_RECT scissors[4] {};
    ComPtr<ID3D11RenderTargetView> rtv[2];
    ComPtr<ID3D11DepthStencilView> dsv;
    ComPtr<ID3D11UnorderedAccessView> uav;
};

Bound Snapshot(ID3D11DeviceContext* c, UINT uavSlot)
{
    Bound b;
    c->IAGetInputLayout(&b.layout);
    c->IAGetPrimitiveTopology(&b.topology);
    c->IAGetVertexBuffers(0, 1, &b.vertexBuffer, &b.vbStride, &b.vbOffset);
    c->IAGetIndexBuffer(&b.indexBuffer, &b.ibFormat, &b.ibOffset);
    UINT n = 0;
    c->VSGetShader(&b.vs, nullptr, &n);
    c->HSGetShader(&b.hs, nullptr, &n);
    c->DSGetShader(&b.ds, nullptr, &n);
    c->GSGetShader(&b.gs, nullptr, &n);
    c->PSGetShader(&b.ps, nullptr, &n);
    c->PSGetShaderResources(0, 1, &b.srv);
    c->RSGetState(&b.rasterizer);
    c->OMGetBlendState(&b.blend, b.blendFactor, &b.sampleMask);
    c->OMGetDepthStencilState(&b.depthState, &b.stencilRef);
    b.viewportCount = 4;
    c->RSGetViewports(&b.viewportCount, b.viewports);
    b.scissorCount = 4;
    c->RSGetScissorRects(&b.scissorCount, b.scissors);

    ID3D11RenderTargetView* rtvs[2] {};
    c->OMGetRenderTargets(2, rtvs, &b.dsv);

    for (int i = 0; i < 2; ++i)
        b.rtv[i].Attach(rtvs[i]);

    ID3D11UnorderedAccessView* uavs[1] {};
    c->OMGetRenderTargetsAndUnorderedAccessViews(0, nullptr, nullptr, uavSlot, 1, uavs);
    b.uav.Attach(uavs[0]);
    return b;
}

bool Same(const Bound& a, const Bound& b, std::string* what)
{
#define SAME(field)                                                                                                    \
    if (a.field != b.field)                                                                                            \
    {                                                                                                                  \
        *what = #field;                                                                                                \
        return false;                                                                                                  \
    }
    SAME(layout.Get())
    SAME(topology)
    SAME(vertexBuffer.Get())
    SAME(vbStride)
    SAME(vbOffset)
    SAME(indexBuffer.Get())
    SAME(ibFormat)
    SAME(ibOffset)
    SAME(vs.Get())
    SAME(hs.Get())
    SAME(ds.Get())
    SAME(gs.Get())
    SAME(ps.Get())
    SAME(srv.Get())
    SAME(rasterizer.Get())
    SAME(blend.Get())
    SAME(sampleMask)
    SAME(depthState.Get())
    SAME(stencilRef)
    SAME(viewportCount)
    SAME(scissorCount)
    SAME(rtv[0].Get())
    SAME(rtv[1].Get())
    SAME(dsv.Get())
    SAME(uav.Get())
#undef SAME

    for (int i = 0; i < 4; ++i)
    {
        if (a.blendFactor[i] != b.blendFactor[i])
        {
            *what = "blendFactor";
            return false;
        }
    }

    for (UINT i = 0; i < a.viewportCount; ++i)
    {
        if (std::memcmp(&a.viewports[i], &b.viewports[i], sizeof(D3D11_VIEWPORT)) != 0)
        {
            *what = "viewport";
            return false;
        }
    }

    for (UINT i = 0; i < a.scissorCount; ++i)
    {
        if (std::memcmp(&a.scissors[i], &b.scissors[i], sizeof(D3D11_RECT)) != 0)
        {
            *what = "scissor";
            return false;
        }
    }

    return true;
}

void StateCases(Device& d, native::MsaaPictureDx11& pic)
{
    const auto vsCode = Compile("float4 main(float4 p : POSITION) : SV_Position { return p; }", "main", "vs_5_0");
    const auto gsCode = Compile("struct V { float4 p : SV_Position; };\n"
                                "[maxvertexcount(3)] void main(triangle V i[3], inout TriangleStream<V> o) {"
                                " for (int k = 0; k < 3; ++k) o.Append(i[k]); }",
                                "main", "gs_5_0");
    const auto hsCode = Compile("struct CP { float4 p : POSITION; };\n"
                                "struct HC { float e[3] : SV_TessFactor; float i : SV_InsideTessFactor; };\n"
                                "HC pc(InputPatch<CP, 3> p) { HC h; h.e[0] = h.e[1] = h.e[2] = 1; h.i = 1; return h; }\n"
                                "[domain(\"tri\")][partitioning(\"integer\")][outputtopology(\"triangle_cw\")]"
                                "[outputcontrolpoints(3)][patchconstantfunc(\"pc\")]\n"
                                "CP main(InputPatch<CP, 3> p, uint i : SV_OutputControlPointID) { return p[i]; }",
                                "main", "hs_5_0");
    const auto dsCode = Compile("struct CP { float4 p : POSITION; };\n"
                                "struct HC { float e[3] : SV_TessFactor; float i : SV_InsideTessFactor; };\n"
                                "[domain(\"tri\")] float4 main(HC h, float3 b : SV_DomainLocation, const OutputPatch<CP, 3> p) "
                                ": SV_Position { return p[0].p * b.x + p[1].p * b.y + p[2].p * b.z; }",
                                "main", "ds_5_0");
    const auto psCode = Compile("float4 main(float4 p : SV_Position) : SV_Target { return p; }", "main", "ps_5_0");

    if (vsCode == nullptr || gsCode == nullptr || hsCode == nullptr || dsCode == nullptr || psCode == nullptr)
    {
        Check(false, "state: compiled the game's stand-in shaders");
        return;
    }

    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11GeometryShader> gs;
    ComPtr<ID3D11HullShader> hs;
    ComPtr<ID3D11DomainShader> ds;
    ComPtr<ID3D11PixelShader> ps;
    ComPtr<ID3D11InputLayout> layout;
    const D3D11_INPUT_ELEMENT_DESC element = { "POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0,
                                               D3D11_INPUT_PER_VERTEX_DATA, 0 };
    const bool shaders =
        SUCCEEDED(d.device->CreateVertexShader(vsCode->GetBufferPointer(), vsCode->GetBufferSize(), nullptr, &vs)) &&
        SUCCEEDED(d.device->CreateGeometryShader(gsCode->GetBufferPointer(), gsCode->GetBufferSize(), nullptr, &gs)) &&
        SUCCEEDED(d.device->CreateHullShader(hsCode->GetBufferPointer(), hsCode->GetBufferSize(), nullptr, &hs)) &&
        SUCCEEDED(d.device->CreateDomainShader(dsCode->GetBufferPointer(), dsCode->GetBufferSize(), nullptr, &ds)) &&
        SUCCEEDED(d.device->CreatePixelShader(psCode->GetBufferPointer(), psCode->GetBufferSize(), nullptr, &ps)) &&
        SUCCEEDED(d.device->CreateInputLayout(&element, 1, vsCode->GetBufferPointer(), vsCode->GetBufferSize(), &layout));
    Check(shaders, "state: made the game's stand-in shaders");

    if (!shaders)
        return;

    D3D11_BUFFER_DESC bufDesc {};
    bufDesc.ByteWidth = 64;
    bufDesc.BindFlags = D3D11_BIND_VERTEX_BUFFER | D3D11_BIND_INDEX_BUFFER;
    ComPtr<ID3D11Buffer> buffer;

    D3D11_RASTERIZER_DESC rsDesc {};
    rsDesc.FillMode = D3D11_FILL_WIREFRAME;
    rsDesc.CullMode = D3D11_CULL_FRONT;
    rsDesc.ScissorEnable = TRUE;
    rsDesc.DepthClipEnable = TRUE;
    D3D11_BLEND_DESC blendDesc {};
    blendDesc.RenderTarget[0].BlendEnable = TRUE;
    blendDesc.RenderTarget[0].SrcBlend = D3D11_BLEND_BLEND_FACTOR;
    blendDesc.RenderTarget[0].DestBlend = D3D11_BLEND_INV_BLEND_FACTOR;
    blendDesc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    blendDesc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    blendDesc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
    blendDesc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    blendDesc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_GREEN;
    D3D11_DEPTH_STENCIL_DESC dsDesc {};
    dsDesc.DepthEnable = TRUE;
    dsDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    dsDesc.DepthFunc = D3D11_COMPARISON_GREATER;
    dsDesc.StencilEnable = TRUE;
    dsDesc.StencilReadMask = dsDesc.StencilWriteMask = 0xff;
    dsDesc.FrontFace = { D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_REPLACE, D3D11_COMPARISON_ALWAYS };
    dsDesc.BackFace = dsDesc.FrontFace;

    ComPtr<ID3D11RasterizerState> rasterizer;
    ComPtr<ID3D11BlendState> blend;
    ComPtr<ID3D11DepthStencilState> depthState;
    auto picture = MakePicture(d, DXGI_FORMAT_R8G8B8A8_UNORM, PictureBits(kFormats[1], 1), 4);
    auto backBuffer = MakeTexture(d, DXGI_FORMAT_R8G8B8A8_UNORM, 4, D3D11_BIND_RENDER_TARGET);
    auto otherTarget = MakeTexture(d, DXGI_FORMAT_R8G8B8A8_UNORM, 1, D3D11_BIND_RENDER_TARGET);
    auto secondTarget = MakeTexture(d, DXGI_FORMAT_R8G8B8A8_UNORM, 1, D3D11_BIND_RENDER_TARGET);
    auto depth = MakeTexture(d, DXGI_FORMAT_D32_FLOAT, 1, D3D11_BIND_DEPTH_STENCIL);
    auto uavTexture = MakeTexture(d, DXGI_FORMAT_R32_UINT, 1, D3D11_BIND_UNORDERED_ACCESS);
    ComPtr<ID3D11RenderTargetView> rtv0, rtv1;
    ComPtr<ID3D11DepthStencilView> dsv;
    ComPtr<ID3D11UnorderedAccessView> uav;
    ComPtr<ID3D11ShaderResourceView> srv;

    const bool objects = picture != nullptr && backBuffer != nullptr && otherTarget != nullptr && secondTarget != nullptr &&
                         depth != nullptr && uavTexture != nullptr &&
                         SUCCEEDED(d.device->CreateBuffer(&bufDesc, nullptr, &buffer)) &&
                         SUCCEEDED(d.device->CreateRasterizerState(&rsDesc, &rasterizer)) &&
                         SUCCEEDED(d.device->CreateBlendState(&blendDesc, &blend)) &&
                         SUCCEEDED(d.device->CreateDepthStencilState(&dsDesc, &depthState)) &&
                         SUCCEEDED(d.device->CreateRenderTargetView(otherTarget.Get(), nullptr, &rtv0)) &&
                         SUCCEEDED(d.device->CreateRenderTargetView(secondTarget.Get(), nullptr, &rtv1)) &&
                         SUCCEEDED(d.device->CreateDepthStencilView(depth.Get(), nullptr, &dsv)) &&
                         SUCCEEDED(d.device->CreateUnorderedAccessView(uavTexture.Get(), nullptr, &uav)) &&
                         SUCCEEDED(d.device->CreateShaderResourceView(picture.Get(), nullptr, &srv));
    Check(objects, "state: made the game's stand-in objects");

    if (!objects)
        return;

    // RT0 and RT1 are single-sample and the size of the default (the textures are 67x35); the depth view matches them.
    for (UINT uavSlot : { 0u, 3u })
    {
        const bool withUav = uavSlot != 0;
        char label[160];
        UINT stride = 16, offset = 0;
        ID3D11Buffer* vertexBuffer = buffer.Get();
        const FLOAT factor[4] = { 0.1f, 0.2f, 0.3f, 0.4f };
        D3D11_VIEWPORT viewports[2] = { { 1, 2, 30, 20, 0.1f, 0.9f }, { 5, 6, 7, 8, 0.2f, 0.7f } };
        D3D11_RECT scissors[2] = { { 1, 2, 30, 20 }, { 3, 4, 9, 10 } };
        ID3D11RenderTargetView* targets[2] = { rtv0.Get(), rtv1.Get() };
        ID3D11ShaderResourceView* read = srv.Get();
        ID3D11UnorderedAccessView* uavs[1] = { uav.Get() };
        const UINT keep = (UINT) -1;

        d.context->IASetInputLayout(layout.Get());
        d.context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST);
        d.context->IASetVertexBuffers(0, 1, &vertexBuffer, &stride, &offset);
        d.context->IASetIndexBuffer(buffer.Get(), DXGI_FORMAT_R16_UINT, 8);
        d.context->VSSetShader(vs.Get(), nullptr, 0);
        d.context->HSSetShader(hs.Get(), nullptr, 0);
        d.context->DSSetShader(ds.Get(), nullptr, 0);
        d.context->GSSetShader(gs.Get(), nullptr, 0);
        d.context->PSSetShader(ps.Get(), nullptr, 0);
        d.context->PSSetShaderResources(0, 1, &read);
        d.context->RSSetState(rasterizer.Get());
        d.context->RSSetViewports(2, viewports);
        d.context->RSSetScissorRects(2, scissors);
        d.context->OMSetBlendState(blend.Get(), factor, 0x5);
        d.context->OMSetDepthStencilState(depthState.Get(), 7);

        if (withUav)
            d.context->OMSetRenderTargetsAndUnorderedAccessViews(1, targets, dsv.Get(), uavSlot, 1, uavs, &keep);
        else
            d.context->OMSetRenderTargets(2, targets, dsv.Get());

        const Bound before = Snapshot(d.context.Get(), uavSlot);
        const char* why = pic.WriteBack(d.context.Get(), picture.Get(), backBuffer.Get());
        const Bound after = Snapshot(d.context.Get(), uavSlot);
        std::string what;
        const bool same = Same(before, after, &what);
        std::snprintf(label, sizeof(label), "state%s: WriteBack puts back everything it touched%s%s",
                      withUav ? " (render target 0 and a pixel shader UAV)" : " (two render targets)",
                      same ? "" : ", differs in ", same ? "" : what.c_str());
        Check(why == nullptr && same, label);

        // The snapshot is only worth anything if what it read is what was set
        Check(before.rasterizer.Get() == rasterizer.Get() && before.blend.Get() == blend.Get() && before.ps.Get() == ps.Get() &&
                  before.hs.Get() == hs.Get() && before.dsv.Get() == dsv.Get() && before.rtv[0].Get() == rtv0.Get() &&
                  (withUav ? before.uav.Get() == uav.Get() : before.rtv[1].Get() == rtv1.Get()),
              "state: the snapshot saw what the test bound");
        d.context->ClearState();
    }
}

void Refusals(Device& d, native::MsaaPictureDx11& pic)
{
    auto msaa = MakeTexture(d, DXGI_FORMAT_B8G8R8A8_UNORM, 4, D3D11_BIND_RENDER_TARGET);
    auto single = MakeTexture(d, DXGI_FORMAT_B8G8R8A8_UNORM, 1, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET);
    auto smaller = MakeTexture(d, DXGI_FORMAT_B8G8R8A8_UNORM, 1, D3D11_BIND_SHADER_RESOURCE, kWidth - 1, kHeight);
    auto otherFamily = MakeTexture(d, DXGI_FORMAT_R16G16B16A16_FLOAT, 1, D3D11_BIND_SHADER_RESOURCE);
    auto noRead = MakeTexture(d, DXGI_FORMAT_B8G8R8A8_UNORM, 1, D3D11_BIND_RENDER_TARGET);
    Check(msaa && single && smaller && otherFamily && noRead, "refusals: made the textures");

    if (!(msaa && single && smaller && otherFamily && noRead))
        return;

    Check(pic.ResolveIn(d.context.Get(), single.Get(), single.Get()) != nullptr, "ResolveIn refuses a single-sample source");
    Check(pic.ResolveIn(d.context.Get(), msaa.Get(), msaa.Get()) != nullptr, "ResolveIn refuses a multisampled destination");
    Check(pic.ResolveIn(d.context.Get(), msaa.Get(), smaller.Get()) != nullptr, "ResolveIn refuses a destination of another size");
    Check(pic.ResolveIn(d.context.Get(), msaa.Get(), otherFamily.Get()) != nullptr,
          "ResolveIn refuses a destination of another format family");
    Check(pic.WriteBack(d.context.Get(), msaa.Get(), msaa.Get()) != nullptr, "WriteBack refuses a multisampled picture");
    Check(pic.WriteBack(d.context.Get(), single.Get(), single.Get()) != nullptr, "WriteBack refuses a single-sample target");
    Check(pic.WriteBack(d.context.Get(), smaller.Get(), msaa.Get()) != nullptr, "WriteBack refuses a picture of another size");
    Check(pic.WriteBack(d.context.Get(), noRead.Get(), msaa.Get()) != nullptr,
          "WriteBack refuses a picture a shader cannot read");

    ComPtr<ID3D11DeviceContext> deferred;
    Check(SUCCEEDED(d.device->CreateDeferredContext(0, &deferred)), "refusals: made a deferred context");

    if (deferred != nullptr)
    {
        Check(pic.ResolveIn(deferred.Get(), msaa.Get(), single.Get()) != nullptr, "ResolveIn refuses a deferred context");
        Check(pic.WriteBack(deferred.Get(), single.Get(), msaa.Get()) != nullptr, "WriteBack refuses a deferred context");
    }
}

// A second device gets its own pipeline, and the back buffer is free to be released after WriteBack (no view of it is kept).
void DeviceAndRelease(Device& a, Device& b, Painter& painterA, native::MsaaPictureDx11& pic)
{
    (void) painterA;
    auto bitsPic = PictureBits(kFormats[0], 9);
    auto pictureB = MakePicture(b, DXGI_FORMAT_B8G8R8A8_UNORM, bitsPic, 4);
    auto backBufferB = MakeTexture(b, DXGI_FORMAT_B8G8R8A8_UNORM, 4, D3D11_BIND_RENDER_TARGET);
    auto pictureA = MakePicture(a, DXGI_FORMAT_B8G8R8A8_UNORM, bitsPic, 4);
    auto backBufferA = MakeTexture(a, DXGI_FORMAT_B8G8R8A8_UNORM, 4, D3D11_BIND_RENDER_TARGET);

    Check(pic.WriteBack(a.context.Get(), pictureA.Get(), backBufferA.Get()) == nullptr, "device A: WriteBack");
    Check(pic.WriteBack(b.context.Get(), pictureB.Get(), backBufferB.Get()) == nullptr, "device B: WriteBack after device A");
    Check(pic.WriteBack(a.context.Get(), pictureA.Get(), backBufferA.Get()) == nullptr, "device A: WriteBack again");

    // Nothing but this test's own reference may be left on the back buffer.
    backBufferA->AddRef();
    const ULONG count = backBufferA->Release();
    Check(count == 1, "WriteBack keeps no reference to the back buffer");

    // The swap chain can be resized after a write-back: no view of its buffer is kept.
    {
        BackBuffer chain;
        const bool made = chain.Make(a, DXGI_FORMAT_B8G8R8A8_UNORM, 2);
        const char* why = made ? pic.WriteBack(a.context.Get(), pictureA.Get(), chain.tex.Get()) : "no swap chain";
        a.context->Flush();
        chain.tex.Reset();
        a.context->ClearState();
        const HRESULT hr = made ? chain.chain->ResizeBuffers(0, 0, 0, DXGI_FORMAT_UNKNOWN, 0) : E_FAIL;
        Check(made && why == nullptr && SUCCEEDED(hr), "a swap chain resizes after a WriteBack into its buffer");
    }

    pic.Release();
    Check(pic.WriteBack(a.context.Get(), pictureA.Get(), backBufferA.Get()) == nullptr, "WriteBack after Release");
}
} // namespace

int main()
{
    Device a, b;

    if (!MakeDevice(&a) || !MakeDevice(&b))
    {
        std::printf("no D3D11 device\n");
        return 2;
    }

    std::printf("debug layer: %s\n", a.info != nullptr ? "on" : "not installed (its check is skipped)");

    Painter painter;

    if (!painter.Make(a))
    {
        std::printf("FAIL  the test's own pipeline could not be made\n");
        return 1;
    }

    native::MsaaPictureDx11 pic;

    ResolveCases(a, painter, pic);
    Check(DebugMessages(a) == 0, "resolve: the debug layer reported nothing");
    WriteBackCases(a, painter, pic);
    Check(DebugMessages(a) == 0, "write back: the debug layer reported nothing");
    StateCases(a, pic);
    Check(DebugMessages(a) == 0, "state: the debug layer reported nothing");
    Refusals(a, pic);
    Check(DebugMessages(a) == 0, "refusals: the debug layer reported nothing");
    DeviceAndRelease(a, b, painter, pic);
    Check(DebugMessages(a) == 0 && DebugMessages(b) == 0, "devices: the debug layer reported nothing");

    std::printf("%s (%d failed)\n", g_failures == 0 ? "PASS" : "FAIL", g_failures);
    return g_failures == 0 ? 0 : 1;
}
