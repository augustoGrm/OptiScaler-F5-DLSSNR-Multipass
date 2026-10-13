// Headless shader test: Windows D3D11 WARP runs the experimental input styles ([DlssNr] InputStyle) on the picture
// the model is shown.
//
// Watercolour (1, DlssNrMode_Watercolor, a Kuwahara filter):
//   1. Flat areas come back unchanged.
//   2. A hard edge stays exactly where it is, unblurred.
//   3. Texture is flattened: its spread falls well below the original's (printed).
//   4. Strength 0 returns the picture bit for bit.
// Structure hint (2, DlssNrMode_StructureHint, a CAS-like local contrast boost on luminance):
//   5. Flat areas unchanged, strength 0 bit for bit.
//   6. Texture spread rises (printed).
//   7. Bounded halos: every pixel's luma stays within its 3x3 neighbourhood's original min and max, widened by a
//      quarter of that range (at full strength).
//   8. Colour kept: a coloured texture's channel ratios are unchanged where no channel reached 1.
//
// Every curve the model can be shown (soft knee, Neutwo, hybrid, HLG, PQ, Linear) and a tone-mapped (passthrough) frame.
//
// Build: cl /nologo /std:c++20 /EHsc /W3 tests\nr_input_style_shader_smoke.cpp /link d3d11.lib d3dcompiler.lib
// Run:   nr_input_style_shader_smoke.exe <dlssnr.hlsl>
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>
#include "../OptiScaler/shaders/dlssnr/DlssNr_Common.h"
using Microsoft::WRL::ComPtr;

struct Pixel { float r, g, b, a; };

struct Image
{
    UINT w = 0, h = 0;
    std::vector<Pixel> px;
    Pixel& at(UINT x, UINT y) { return px[y * w + x]; }
    const Pixel& at(UINT x, UINT y) const { return px[y * w + x]; }
};

static void check(HRESULT hr, int line = __builtin_LINE())
{
    if (FAILED(hr)) throw std::runtime_error("D3D call failed, line " + std::to_string(line));
}
static int fails = 0;
static void expect(bool ok, const std::string& label) { if (!ok) { std::printf("FAIL: %s\n", label.c_str()); ++fails; } }

struct Gpu
{
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> ctx;
    ComPtr<ID3D11SamplerState> linearSampler;

    Gpu()
    {
        check(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &ctx));
        D3D11_SAMPLER_DESC sampling {};
        sampling.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sampling.AddressU = sampling.AddressV = sampling.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sampling.MaxLOD = D3D11_FLOAT32_MAX;
        check(device->CreateSamplerState(&sampling, &linearSampler));
    }

    ComPtr<ID3D11ComputeShader> Compile(const wchar_t* path)
    {
        ComPtr<ID3DBlob> code, errors;
        // WARP crashes running the shader with the percentile exposure meter in (dlssnr.hlsl); not tested here
        const D3D_SHADER_MACRO defines[] = { { "DLSSNR_NO_PERCENTILE_METER", "1" }, { nullptr, nullptr } };
        HRESULT hr = D3DCompileFromFile(path, defines, D3D_COMPILE_STANDARD_FILE_INCLUDE, "CSMain", "cs_5_0",
                                        D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
        if (errors && FAILED(hr)) std::fprintf(stderr, "%s", (char*) errors->GetBufferPointer());
        check(hr);
        ComPtr<ID3D11ComputeShader> shader;
        check(device->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &shader));
        return shader;
    }

    ComPtr<ID3D11Texture2D> Texture(UINT w, UINT h, const Image* data)
    {
        D3D11_TEXTURE2D_DESC desc {};
        desc.Width = w; desc.Height = h; desc.MipLevels = 1; desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT; desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
        D3D11_SUBRESOURCE_DATA init {data ? data->px.data() : nullptr, (UINT) (w * sizeof(Pixel)), 0};
        ComPtr<ID3D11Texture2D> texture;
        check(device->CreateTexture2D(&desc, data ? &init : nullptr, &texture));
        return texture;
    }

    Image Read(ID3D11Texture2D* texture)
    {
        D3D11_TEXTURE2D_DESC desc {};
        texture->GetDesc(&desc);
        desc.BindFlags = 0; desc.Usage = D3D11_USAGE_STAGING; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> staging;
        check(device->CreateTexture2D(&desc, nullptr, &staging));
        ctx->CopyResource(staging.Get(), texture);
        D3D11_MAPPED_SUBRESOURCE mapped {};
        check(ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped));
        Image image {desc.Width, desc.Height, std::vector<Pixel>(desc.Width * desc.Height)};
        for (UINT y = 0; y < desc.Height; ++y)
            std::memcpy(&image.px[y * desc.Width], (char*) mapped.pData + y * mapped.RowPitch, desc.Width * sizeof(Pixel));
        ctx->Unmap(staging.Get(), 0);
        return image;
    }

    // One compute dispatch over w x h in 8x8 groups: t0 (and t1..t4 = t0), u0 target, u1 spare.
    template <typename Constants>
    void Run(ID3D11ComputeShader* shader, const Constants& k, ID3D11Texture2D* t0, ID3D11Texture2D* u0, ID3D11Texture2D* u1,
             UINT w, UINT h)
    {
        D3D11_BUFFER_DESC desc {};
        desc.ByteWidth = (UINT) ((sizeof(Constants) + 15) / 16 * 16);
        desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        std::vector<uint8_t> bytes(desc.ByteWidth);
        std::memcpy(bytes.data(), &k, sizeof(Constants));
        D3D11_SUBRESOURCE_DATA init {bytes.data(), 0, 0};
        ComPtr<ID3D11Buffer> constants;
        check(device->CreateBuffer(&desc, &init, &constants));
        ComPtr<ID3D11ShaderResourceView> s0;
        check(device->CreateShaderResourceView(t0, nullptr, &s0));
        ComPtr<ID3D11UnorderedAccessView> v0, v1;
        check(device->CreateUnorderedAccessView(u0, nullptr, &v0));
        if (u1) check(device->CreateUnorderedAccessView(u1, nullptr, &v1));
        ID3D11ShaderResourceView* srvs[] = {s0.Get(), s0.Get(), s0.Get(), s0.Get(), s0.Get()};
        ID3D11UnorderedAccessView* uavs[] = {v0.Get(), v1.Get()};
        ctx->CSSetShader(shader, nullptr, 0);
        ctx->CSSetShaderResources(0, 5, srvs);
        ctx->CSSetUnorderedAccessViews(0, u1 ? 2 : 1, uavs, nullptr);
        ctx->CSSetConstantBuffers(0, 1, constants.GetAddressOf());
        ctx->CSSetSamplers(0, 1, linearSampler.GetAddressOf());
        ctx->Dispatch((w + 7) / 8, (h + 7) / 8, 1);
        ID3D11ShaderResourceView* noSrvs[5] = {};
        ID3D11UnorderedAccessView* noUavs[2] = {};
        ctx->CSSetShaderResources(0, 5, noSrvs);
        ctx->CSSetUnorderedAccessViews(0, 2, noUavs, nullptr);
    }
};

static DlssNrConstants Base(uint32_t mode, UINT w, UINT h, uint32_t passthrough, uint32_t reversible)
{
    DlssNrConstants k {};
    k.Mode = mode;
    k.Width = w; k.Height = h;
    k.WhitePoint = 1.0f;
    k.TransferStrength = 1.0f; k.ColourStrength = 1.0f;
    k.MaxRatio = 8.0f;
    k.Passthrough = passthrough;
    k.ApplyModel = 1;
    k.ReversibleMode = reversible;
    k.DebugScale = 1.0f;
    k.ModelWorkScale = 1.0f;
    k.PassFeedback = 1.0f;
    return k;
}

// The encode's proxy (the picture the model would be shown) and its watercolour at a strength.
struct Pictures { Image proxy, painted; };

static Pictures Paint(Gpu& gpu, ID3D11ComputeShader* nr, const Image& frame, uint32_t passthrough, uint32_t reversible,
                      float strength, uint32_t mode = DlssNrMode_Watercolor, bool facesOnly = false)
{
    const UINT w = frame.w, h = frame.h;
    auto source = gpu.Texture(w, h, &frame);
    auto proxy = gpu.Texture(w, h, nullptr);
    auto keep = gpu.Texture(w, h, nullptr);
    auto painted = gpu.Texture(w, h, nullptr);
    gpu.Run(nr, Base(DlssNrMode_Encode, w, h, passthrough, reversible), source.Get(), proxy.Get(), keep.Get(), w, h);
    DlssNrConstants k = Base(mode, w, h, passthrough, reversible);
    k.TransferStrength = strength;
    k.SkinProtection = facesOnly ? 1u : 0u; // the style passes read it as "faces only"
    gpu.Run(nr, k, proxy.Get(), painted.Get(), keep.Get(), w, h);
    return {gpu.Read(proxy.Get()), gpu.Read(painted.Get())};
}

static float Luma(const Pixel& p) { return 0.2126f * p.r + 0.7152f * p.g + 0.0722f * p.b; }

// 64 x 64: a flat left band, a hard vertical edge (dark | bright) at x = 32 in the middle band, and fine texture
// (pixel grain and a 4-pixel weave) on the right.
static const UINT kSize = 64;

static Image Scene()
{
    Image image {kSize, kSize, std::vector<Pixel>(kSize * kSize)};
    uint32_t seed = 12345;
    for (UINT y = 0; y < kSize; ++y)
        for (UINT x = 0; x < kSize; ++x)
        {
            float v = 0.25f;
            if (x >= 16 && x < 44)
                v = x < 32 ? 0.03f : 0.9f;
            else if (x >= 44)
            {
                seed = seed * 1664525u + 1013904223u;
                const float grain = ((seed >> 8) & 0xFFFF) / 32767.5f - 1.0f;
                const float weave = std::sin(1.5707963f * (x + 0.5f)) * std::sin(1.5707963f * (y + 0.5f) + 0.7f);
                v = 0.3f * (1.0f + 0.2f * (y < kSize / 2 ? grain : weave));
            }
            image.at(x, y) = {v, v, v, 1};
        }
    return image;
}

int wmain(int argc, wchar_t** argv) try
{
    if (argc != 2)
        throw std::runtime_error("Pass the dlssnr.hlsl path");
    Gpu gpu;
    auto nr = gpu.Compile(argv[1]);
    const Image frame = Scene();

    struct Case { uint32_t passthrough, reversible; const char* name; };
    const Case cases[] = {{0, 0, "soft knee"}, {0, 1, "Neutwo"}, {0, 3, "hybrid"}, {0, 5, "HLG"}, {0, 6, "PQ"}, {0, 7, "Linear"},
                          {1, 0, "tone-mapped"}};

    std::printf("%-12s %10s %10s %10s %10s %10s\n", "curve", "flat", "edge", "grain", "weave", "off");
    for (const Case& c : cases)
    {
        const Pictures full = Paint(gpu, nr.Get(), frame, c.passthrough, c.reversible, 1.0f);
        const Pictures none = Paint(gpu, nr.Get(), frame, c.passthrough, c.reversible, 0.0f);

        float flat = 0.0f, edge = 0.0f, off = 0.0f;
        for (UINT y = 0; y < kSize; ++y)
            for (UINT x = 0; x < kSize; ++x)
            {
                const Pixel& a = full.proxy.at(x, y);
                const Pixel& b = full.painted.at(x, y);
                const float d = std::max({std::abs(a.r - b.r), std::abs(a.g - b.g), std::abs(a.b - b.b)});
                if (x >= 3 && x < 13)
                    flat = std::max(flat, d);
                if (x >= 19 && x < 41) // both sides of the edge, including the two pixels touching it
                    edge = std::max(edge, d);
                const Pixel& n = none.painted.at(x, y);
                off = std::max({off, std::abs(n.r - a.r), std::abs(n.g - a.g), std::abs(n.b - a.b)});
            }

        // Texture: standard deviation of the stored luma, after / before, per texture.
        auto spread = [](const Image& im, UINT y0, UINT y1)
        {
            double s = 0.0, s2 = 0.0;
            int n = 0;
            for (UINT y = y0; y < y1; ++y)
                for (UINT x = 47; x < kSize - 3; ++x)
                {
                    const double l = Luma(im.at(x, y));
                    s += l;
                    s2 += l * l;
                    ++n;
                }
            const double m = s / n;
            return std::sqrt(std::max(s2 / n - m * m, 0.0));
        };
        const double grain = spread(full.painted, 3, kSize / 2 - 3) / spread(full.proxy, 3, kSize / 2 - 3);
        const double weave = spread(full.painted, kSize / 2 + 3, kSize - 3) / spread(full.proxy, kSize / 2 + 3, kSize - 3);

        std::printf("%-12s %10.2e %10.2e %9.0f%% %9.0f%% %10.2e\n", c.name, flat, edge, grain * 100.0, weave * 100.0, off);
        const std::string what = std::string(" (") + c.name + ")";
        expect(flat < 1e-4f, "flat areas change" + what);
        expect(edge < 1e-4f, "the hard edge moves or blurs" + what);
        expect(grain < 0.8 && weave < 0.8, "texture is not flattened" + what);
        expect(off == 0.0f, "strength 0 changes the picture" + what);
    }

    // Structure hint.
    std::printf("\n%-12s %10s %10s %10s %10s %10s %10s\n", "curve", "flat", "off", "grain", "weave", "halo", "colour");
    for (const Case& c : cases)
    {
        const Pictures full = Paint(gpu, nr.Get(), frame, c.passthrough, c.reversible, 1.0f, DlssNrMode_StructureHint);
        const Pictures none = Paint(gpu, nr.Get(), frame, c.passthrough, c.reversible, 0.0f, DlssNrMode_StructureHint);

        float flat = 0.0f, off = 0.0f, halo = 0.0f, dark = 0.0f;
        for (UINT y = 0; y < kSize; ++y)
            for (UINT x = 0; x < kSize; ++x)
            {
                const Pixel& a = full.proxy.at(x, y);
                const Pixel& b = full.painted.at(x, y);
                if (x >= 3 && x < 13)
                    flat = std::max({flat, std::abs(a.r - b.r), std::abs(a.g - b.g), std::abs(a.b - b.b)});
                const Pixel& n = none.painted.at(x, y);
                off = std::max({off, std::abs(n.r - a.r), std::abs(n.g - a.g), std::abs(n.b - a.b)});
                float lo = 1e30f, hi = -1e30f;
                for (int dy = -1; dy <= 1; ++dy)
                    for (int dx = -1; dx <= 1; ++dx)
                    {
                        const UINT xx = (UINT) std::clamp((int) x + dx, 0, (int) kSize - 1);
                        const UINT yy = (UINT) std::clamp((int) y + dy, 0, (int) kSize - 1);
                        const float l = Luma(full.proxy.at(xx, yy));
                        lo = std::min(lo, l);
                        hi = std::max(hi, l);
                    }
                const float l = Luma(b);
                const float margin = 0.25f * (hi - lo);
                halo = std::max({halo, l - (hi + margin), (lo - margin) - l});
                // The dark side of the hard edge (0.03 | 0.9): no lower than three quarters of the darkest neighbour.
                dark = std::max(dark, 0.75f * lo - l);
            }
        auto spread = [](const Image& im, UINT y0, UINT y1)
        {
            double s = 0.0, s2 = 0.0;
            int n = 0;
            for (UINT y = y0; y < y1; ++y)
                for (UINT x = 47; x < kSize - 3; ++x)
                {
                    const double l = Luma(im.at(x, y));
                    s += l;
                    s2 += l * l;
                    ++n;
                }
            const double m = s / n;
            return std::sqrt(std::max(s2 / n - m * m, 0.0));
        };
        const double grain = spread(full.painted, 3, kSize / 2 - 3) / spread(full.proxy, 3, kSize / 2 - 3);
        const double weave = spread(full.painted, kSize / 2 + 3, kSize - 3) / spread(full.proxy, kSize / 2 + 3, kSize - 3);

        // Colour: a coloured weave; channel ratios before and after, where the result's peak stayed below 1.
        Image coloured = frame;
        for (Pixel& p : coloured.px)
            p = {p.r * 1.0f, p.g * 0.6f, p.b * 0.3f, 1};
        const Pictures tinted = Paint(gpu, nr.Get(), coloured, c.passthrough, c.reversible, 1.0f, DlssNrMode_StructureHint);
        float colour = 0.0f;
        for (size_t i = 0; i < tinted.proxy.px.size(); ++i)
        {
            const Pixel& a = tinted.proxy.px[i];
            const Pixel& b = tinted.painted.px[i];
            if (a.r < 1e-3f || b.r >= 0.9999f)
                continue;
            colour = std::max({colour, std::abs(a.g / a.r - b.g / b.r), std::abs(a.b / a.r - b.b / b.r)});
        }

        std::printf("%-12s %10.2e %10.2e %9.0f%% %9.0f%% %10.2e %10.2e dark %.2e\n", c.name, flat, off, grain * 100.0,
                    weave * 100.0, halo, colour, dark);
        const std::string what = std::string(" (structure hint, ") + c.name + ")";
        expect(flat < 1e-6f, "flat areas change" + what);
        expect(off == 0.0f, "strength 0 changes the picture" + what);
        expect(grain > 1.05 && weave > 1.05, "texture contrast does not rise" + what);
        expect(halo < 1e-5f, "a pixel overshoots its neighbourhood's range by more than a quarter (halo)" + what);
        expect(dark < 1e-5f, "the dark side of an edge falls below 3/4 of its darkest neighbour (goes to black)" + what);
        expect(colour < 1e-4f, "the colour's channel ratios change" + what);

        // Strength 3: more texture than at 1, overshoot within 0.75 of the range.
        const Pictures strong = Paint(gpu, nr.Get(), frame, c.passthrough, c.reversible, 3.0f, DlssNrMode_StructureHint);
        const double weaveStrong =
            spread(strong.painted, kSize / 2 + 3, kSize - 3) / spread(strong.proxy, kSize / 2 + 3, kSize - 3);
        float haloStrong = 0.0f, darkStrong = 0.0f;
        for (UINT y = 0; y < kSize; ++y)
            for (UINT x = 0; x < kSize; ++x)
            {
                float lo = 1e30f, hi = -1e30f;
                for (int dy = -1; dy <= 1; ++dy)
                    for (int dx = -1; dx <= 1; ++dx)
                    {
                        const UINT xx = (UINT) std::clamp((int) x + dx, 0, (int) kSize - 1);
                        const UINT yy = (UINT) std::clamp((int) y + dy, 0, (int) kSize - 1);
                        const float l = Luma(strong.proxy.at(xx, yy));
                        lo = std::min(lo, l);
                        hi = std::max(hi, l);
                    }
                const float l = Luma(strong.painted.at(x, y));
                haloStrong = std::max({haloStrong, l - (hi + 0.75f * (hi - lo)), (lo - 0.75f * (hi - lo)) - l});
                darkStrong = std::max(darkStrong, 0.25f * lo - l);
            }
        std::printf("%-12s strength 3: weave %.0f%%, overshoot past 0.75 of the range %.2e\n", c.name, weaveStrong * 100.0,
                    haloStrong);
        expect(weaveStrong > weave + 0.2, "strength 3 does not lift texture beyond strength 1" + what);
        expect(haloStrong < 1e-5f, "strength 3 overshoots by more than 0.75 of the range" + what);
        expect(darkStrong < 1e-5f, "strength 3 takes the dark side of an edge below 1/4 of its darkest neighbour" + what);
    }

    // Smooth canvas: flat areas and the hard edge kept, texture flattened (more than the watercolour's grain), strength
    // 0 exact.
    auto spreadOf = [](const Image& im, UINT x0, UINT x1, UINT y0, UINT y1)
    {
        double s = 0.0, s2 = 0.0;
        int n = 0;
        for (UINT y = y0; y < y1; ++y)
            for (UINT x = x0; x < x1; ++x)
            {
                const double l = Luma(im.at(x, y));
                s += l;
                s2 += l * l;
                ++n;
            }
        const double m = s / n;
        return std::sqrt(std::max(s2 / n - m * m, 0.0));
    };

    // Two steps in the scene: the hard edge 0.03 | 0.9 at x = 32, and a smaller one 0.25 | 0.03 at x = 16 (about 3 EV,
    // a strong lip line). The 11x11 window reaches both from different pixels, so they are measured apart: the hard edge must
    // stay exact, the small step may soften a little (the brightness tolerance is wide on purpose, to flatten skin).
    std::printf("\n%-12s %10s %10s %10s %10s %10s %10s\n", "curve", "flat", "edge", "step", "grain", "weave", "off");
    for (const Case& c : cases)
    {
        const Pictures full = Paint(gpu, nr.Get(), frame, c.passthrough, c.reversible, 1.0f, DlssNrMode_SmoothCanvas);
        const Pictures none = Paint(gpu, nr.Get(), frame, c.passthrough, c.reversible, 0.0f, DlssNrMode_SmoothCanvas);

        float flat = 0.0f, edge = 0.0f, step = 0.0f, off = 0.0f;
        for (UINT y = 0; y < kSize; ++y)
            for (UINT x = 0; x < kSize; ++x)
            {
                const Pixel& a = full.proxy.at(x, y);
                const Pixel& b = full.painted.at(x, y);
                const float d = std::max({std::abs(a.r - b.r), std::abs(a.g - b.g), std::abs(a.b - b.b)});
                if (x >= 3 && x < 11)
                    flat = std::max(flat, d);
                if (x >= 22 && x < 39) // both sides of the hard edge, out of reach of the small step and the texture
                    edge = std::max(edge, d);
                if (x >= 11 && x < 22) // both sides of the small step
                    step = std::max(step, d);
                const Pixel& n = none.painted.at(x, y);
                off = std::max({off, std::abs(n.r - a.r), std::abs(n.g - a.g), std::abs(n.b - a.b)});
            }

        const double grain =
            spreadOf(full.painted, 50, kSize - 6, 6, kSize / 2 - 6) / spreadOf(full.proxy, 50, kSize - 6, 6, kSize / 2 - 6);
        const double weave = spreadOf(full.painted, 50, kSize - 6, kSize / 2 + 6, kSize - 6) /
                             spreadOf(full.proxy, 50, kSize - 6, kSize / 2 + 6, kSize - 6);

        std::printf("%-12s %10.2e %10.2e %10.2e %9.0f%% %9.0f%% %10.2e\n", c.name, flat, edge, step, grain * 100.0,
                    weave * 100.0, off);
        const std::string what = std::string(" (smooth canvas, ") + c.name + ")";
        expect(flat < 1e-4f, "flat areas change" + what);
        expect(edge < 1e-3f, "the hard edge moves or blurs" + what);
        expect(step < 0.03f, "the small step softens by more than 0.03" + what);
        expect(grain < 0.3 && weave < 0.3, "texture is not flattened to under 30%" + what);
        expect(off == 0.0f, "strength 0 changes the picture" + what);
    }

    // Faces only: grey texture is left exactly as it was on every curve (no skin colour); skin-coloured texture is
    // smoothed (tone-mapped frame, where the stored values are the display colours the skin test is made for).
    std::printf("\n%-12s %10s %10s\n", "curve", "grey off", "skin");
    for (const Case& c : cases)
    {
        Image mixed = frame;
        for (UINT y = 0; y < kSize; ++y)
            for (UINT x = 0; x < kSize; ++x)
                if (y >= kSize / 2) // the lower half's texture skin-coloured (a warm 0.85 : 0.62 : 0.50 tone)
                {
                    Pixel& p = mixed.at(x, y);
                    p = {p.r * 2.6f * 0.85f, p.g * 2.6f * 0.62f, p.b * 2.6f * 0.50f, 1};
                }

        float greyOff = 0.0f;
        double skin = 0.0;
        for (uint32_t mode : {DlssNrMode_Watercolor, DlssNrMode_StructureHint, DlssNrMode_SmoothCanvas})
        {
            const Pictures faces = Paint(gpu, nr.Get(), mixed, c.passthrough, c.reversible, 1.0f, mode, true);
            for (UINT y = 0; y < kSize / 2; ++y)
                for (UINT x = 0; x < kSize; ++x)
                {
                    const Pixel& a = faces.proxy.at(x, y);
                    const Pixel& b = faces.painted.at(x, y);
                    greyOff = std::max({greyOff, std::abs(a.r - b.r), std::abs(a.g - b.g), std::abs(a.b - b.b)});
                }
            if (mode == DlssNrMode_SmoothCanvas)
                skin = spreadOf(faces.painted, 50, kSize - 6, kSize / 2 + 6, kSize - 6) /
                       spreadOf(faces.proxy, 50, kSize - 6, kSize / 2 + 6, kSize - 6);
        }

        std::printf("%-12s %10.2e %9.0f%%\n", c.name, greyOff, skin * 100.0);
        const std::string what = std::string(" (faces only, ") + c.name + ")";
        expect(greyOff == 0.0f, "a style changes grey (non-skin) texture" + what);
        if (c.passthrough != 0)
            expect(skin < 0.3, "skin-coloured texture is not smoothed" + what);
    }

    if (fails)
    {
        std::printf("%d check(s) failed\n", fails);
        return 1;
    }
    std::puts("PASS: watercolour keeps flat areas and hard edges and flattens texture; the structure hint raises texture\n"
              "      contrast with bounded halos and no colour change; the smooth canvas keeps flat areas and hard edges\n"
              "      and flattens texture; faces only leaves grey texture alone and smooths skin; strength 0 is a no-op\n"
              "      (WARP HLSL)");
    return 0;
}
catch (const std::exception& e)
{
    std::fprintf(stderr, "FAIL: %s\n", e.what());
    return 1;
}
