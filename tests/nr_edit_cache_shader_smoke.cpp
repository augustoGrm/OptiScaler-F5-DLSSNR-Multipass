// Headless shader test: Windows D3D11 WARP runs precompile/dlssnr_edit_cache.hlsl, the edit cache (the model one frame
// in N, its edit carried along the vectors in between). The test drives a short cadence itself, as the host does, on
// small synthetic frames whose right answer is known:
//   1. Ring: a frame's vectors land in their slice as uv displacement (raw x scale / subrect size), the rest untouched;
//      at an edge the vector comes from the nearer surface (depth).
//   2. Capture: the edit is log2((answer + eps) / (frame + eps)) per channel; the fingerprint is log luma and chroma.
//   3. Carry, still scene: the anchor's edit comes back exactly and is believed everywhere.
//   4. Carry, a pan of 1 px a frame over 3 frames: the edit stays on its surface (exact away from the edge it pans in at).
//   5. The fingerprint: a pixel whose brightness changed past the tolerance is dropped; dropped stays dropped on the
//      next frame even where the colour came back (only a refresh restores it).
//   6. Apply: a believed pixel shows frame x 2^edit exactly; a dropped one takes the edit of validated pixels of the same
//      brightness nearby, and none where nothing similar is near.
//   7. Display-encoded frames: a zero edit gives the frame back (sRGB decoded and encoded again).
//   8. Crossfade: the shown edit is the fade source carried here, blended toward the new one by FadeWeight.
//   9. Compose: the vectors back to the anchor in the game's raw units (3 frames of -1 px = -3 px).
// cl /nologo /std:c++20 /EHsc /W4 /wd4324 tests\nr_edit_cache_shader_smoke.cpp d3d11.lib d3dcompiler.lib
// nr_edit_cache_shader_smoke.exe OptiScaler\shaders\dlssnr\precompile\dlssnr_edit_cache.hlsl
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>
#include "../OptiScaler/shaders/dlssnr/DlssNr_EditCacheConstants.h"
using Microsoft::WRL::ComPtr;

static void check(HRESULT hr)
{
    if (FAILED(hr))
        throw std::runtime_error("D3D call failed");
}

static int fails = 0;
static void expect(bool ok, const std::string& label)
{
    if (!ok)
    {
        std::printf("FAIL: %s\n", label.c_str());
        ++fails;
    }
}

struct Px
{
    float r, g, b, a;
};

constexpr unsigned W = 32, H = 16; // output size; the vectors are at the same size unless a case says otherwise

struct Img
{
    unsigned w = W, h = H;
    std::vector<Px> px;
    Px at(unsigned x, unsigned y) const { return px[y * w + x]; }
    Px& at(unsigned x, unsigned y) { return px[y * w + x]; }
};

static Img Fill(const std::function<Px(unsigned, unsigned)>& f, unsigned w = W, unsigned h = H)
{
    Img img { w, h, std::vector<Px>(w * h) };
    for (unsigned y = 0; y < h; ++y)
        for (unsigned x = 0; x < w; ++x)
            img.px[y * w + x] = f(x, y);
    return img;
}

struct Gpu
{
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> ctx;
    ComPtr<ID3D11ComputeShader> shader;
    ComPtr<ID3D11Buffer> constants;
    ComPtr<ID3D11SamplerState> sampler;

    ComPtr<ID3D11Texture2D> Make(const Img& img, UINT bind, D3D11_USAGE usage = D3D11_USAGE_DEFAULT, UINT cpu = 0,
                                 bool upload = true)
    {
        D3D11_TEXTURE2D_DESC desc {};
        desc.Width = img.w;
        desc.Height = img.h;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        desc.SampleDesc.Count = 1;
        desc.Usage = usage;
        desc.BindFlags = bind;
        desc.CPUAccessFlags = cpu;
        D3D11_SUBRESOURCE_DATA init { img.px.data(), img.w * (UINT) sizeof(Px), 0 };
        ComPtr<ID3D11Texture2D> tex;
        check(device->CreateTexture2D(&desc, upload && !img.px.empty() ? &init : nullptr, &tex));
        return tex;
    }

    Img Read(ID3D11Texture2D* tex, unsigned w, unsigned h)
    {
        const auto readback = Make(Img { w, h, {} }, 0, D3D11_USAGE_STAGING, D3D11_CPU_ACCESS_READ, false);
        ctx->CopyResource(readback.Get(), tex);
        D3D11_MAPPED_SUBRESOURCE mapped {};
        check(ctx->Map(readback.Get(), 0, D3D11_MAP_READ, 0, &mapped));
        Img out { w, h, std::vector<Px>(w * h) };
        for (unsigned y = 0; y < h; ++y)
            memcpy(&out.px[y * w], (const char*) mapped.pData + y * mapped.RowPitch, w * sizeof(Px));
        ctx->Unmap(readback.Get(), 0);
        return out;
    }

    // One mode over threadsW x threadsH. u0 and u1 start from the given images (their size is the outputs' size), so a
    // ring or an atlas keeps what earlier runs wrote; both come back.
    void Run(const DlssNrEditCacheConstants& c, const std::vector<const Img*>& inputs, Img& out0, Img& out1,
             unsigned threadsW, unsigned threadsH)
    {
        std::vector<ComPtr<ID3D11ShaderResourceView>> srvs;
        for (const Img* in : inputs)
        {
            const auto tex = Make(*in, D3D11_BIND_SHADER_RESOURCE);
            ComPtr<ID3D11ShaderResourceView> srv;
            check(device->CreateShaderResourceView(tex.Get(), nullptr, &srv));
            srvs.push_back(srv);
        }
        while (srvs.size() < 6)
            srvs.push_back(srvs.front());
        const auto t0 = Make(out0, D3D11_BIND_UNORDERED_ACCESS);
        const auto t1 = Make(out1, D3D11_BIND_UNORDERED_ACCESS);
        ComPtr<ID3D11UnorderedAccessView> uav0, uav1;
        check(device->CreateUnorderedAccessView(t0.Get(), nullptr, &uav0));
        check(device->CreateUnorderedAccessView(t1.Get(), nullptr, &uav1));
        ID3D11ShaderResourceView* views[6];
        for (int i = 0; i < 6; ++i)
            views[i] = srvs[i].Get();
        ID3D11UnorderedAccessView* uavs[] = { uav0.Get(), uav1.Get() };
        ctx->CSSetShader(shader.Get(), nullptr, 0);
        ctx->CSSetShaderResources(0, 6, views);
        ctx->CSSetUnorderedAccessViews(0, 2, uavs, nullptr);
        ctx->CSSetConstantBuffers(0, 1, constants.GetAddressOf());
        ctx->CSSetSamplers(0, 1, sampler.GetAddressOf());
        ctx->UpdateSubresource(constants.Get(), 0, nullptr, &c, 0, 0);
        ctx->Dispatch((threadsW + 7) / 8, (threadsH + 7) / 8, 1);
        ID3D11UnorderedAccessView* none[] = { nullptr, nullptr };
        ctx->CSSetUnorderedAccessViews(0, 2, none, nullptr);
        out0 = Read(t0.Get(), out0.w, out0.h);
        out1 = Read(t1.Get(), out1.w, out1.h);
    }
};

static Img Blank(unsigned w, unsigned h) { return Fill([](unsigned, unsigned) { return Px { 0, 0, 0, 0 }; }, w, h); }
static float Luma(Px p) { return 0.2126f * p.r + 0.7152f * p.g + 0.0722f * p.b; }
static bool Near(float a, float b, float e) { return std::abs(a - b) <= e; }

// The scene: a texture that pans right by `shift` pixels, its edit fixed to it (stops, varies by channel and place).
static Px Texture(float sx, unsigned y) { return { 0.30f + 0.10f * std::sin(sx * 0.7f), 0.25f + 0.05f * (y % 3), 0.20f, 1 }; }
static Px EditAt(float sx, unsigned y) { return { 0.4f * std::sin(sx * 0.3f), -0.2f + 0.02f * y, 0.1f * std::cos(sx * 0.5f), 0 }; }
static Img Frame(int shift) { return Fill([=](unsigned x, unsigned y) { return Texture((float) x - shift, y); }); }
static Img Answer(int shift)
{
    return Fill([=](unsigned x, unsigned y)
                {
                    const Px f = Texture((float) x - shift, y), e = EditAt((float) x - shift, y);
                    const float eps = 1.0f / 512.0f;
                    return Px { (f.r + eps) * std::exp2(e.r) - eps, (f.g + eps) * std::exp2(e.g) - eps,
                                (f.b + eps) * std::exp2(e.b) - eps, 1 };
                });
}

static DlssNrEditCacheConstants Base(DlssNrEditCacheMode mode)
{
    DlssNrEditCacheConstants c {};
    c.Mode = mode;
    c.Width = W;
    c.Height = H;
    c.MotionWidth = W;
    c.MotionHeight = H;
    c.MvScaleX = 1.0f;
    c.MvScaleY = 1.0f;
    c.Epsilon = 1.0f / 512.0f;
    c.Tolerance = kDlssNrEditCacheTolerance;
    c.ContextTolerance = kDlssNrEditCacheContextTolerance;
    c.FillSigma = kDlssNrEditCacheFillSigma;
    c.FadeWeight = 1.0f;
    for (uint32_t level = 0; level < kDlssNrEditCacheCellLevels; ++level)
    {
        const uint32_t size = kDlssNrEditCacheCellSize[level];
        (&c.CellCountX0)[level] = (W + size - 1) / size;
        (&c.CellCountY0)[level] = (H + size - 1) / size;
    }
    c.CellAtlasX0 = 0;
    c.CellAtlasX1 = c.CellCountX0;
    c.CellAtlasX2 = c.CellCountX0 + c.CellCountX1;
    return c;
}

static unsigned AtlasWidth(const DlssNrEditCacheConstants& c) { return c.CellCountX0 + c.CellCountX1 + c.CellCountX2; }

int wmain(int argc, wchar_t** argv)
try
{
    if (argc < 2)
        throw std::runtime_error("usage: nr_edit_cache_shader_smoke <dlssnr_edit_cache.hlsl>");

    Gpu gpu;
    check(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &gpu.device,
                            nullptr, &gpu.ctx));
    ComPtr<ID3DBlob> blob, errors;
    if (FAILED(D3DCompileFromFile(argv[1], nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE, "CSMain", "cs_5_0", 0, 0, &blob,
                                  &errors)))
        throw std::runtime_error(errors ? (const char*) errors->GetBufferPointer() : "compile failed");
    check(gpu.device->CreateComputeShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &gpu.shader));
    D3D11_BUFFER_DESC cb { 256, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0 };
    check(gpu.device->CreateBuffer(&cb, nullptr, &gpu.constants));
    D3D11_SAMPLER_DESC sd {};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    check(gpu.device->CreateSamplerState(&sd, &gpu.sampler));

    const Img zeroDepth = Blank(W, H);
    Img scratch = Blank(W, H);

    // 1. Ring: slice 2 gets this frame's displacement, the other slices keep what they had
    {
        Img ring = Fill([](unsigned, unsigned) { return Px { 9, 9, 0, 0 }; }, W, H * kDlssNrEditCacheRingSlices);
        const Img motion = Fill([](unsigned, unsigned) { return Px { -2.0f, 1.0f, 0, 0 }; });
        auto c = Base(DlssNrEditCache_Ring);
        c.RingSlice = 2;
        c.MvScaleX = 0.5f; // the game's scale: -2 raw = -1 pixel
        gpu.Run(c, { &motion, &zeroDepth }, ring, scratch, W, H);
        bool slice = true, others = true;
        for (unsigned y = 0; y < ring.h; ++y)
            for (unsigned x = 0; x < W; ++x)
            {
                const Px p = ring.at(x, y);
                if (y / H == 2)
                    slice = slice && Near(p.r, -1.0f / W, 1e-7f) && Near(p.g, 1.0f / H, 1e-7f);
                else
                    others = others && p.r == 9 && p.g == 9;
            }
        expect(slice, "ring: the frame's vectors land in their slice as uv displacement");
        expect(others, "ring: the other slices are untouched");

        // An edge: the left half moves 3 px, the right half (nearer, reversed Z) moves 1 px; the texel just left of the
        // edge takes the nearer surface's vector from its 3x3
        const Img edgeMotion = Fill([](unsigned x, unsigned) { return Px { x < 16 ? -3.0f : -1.0f, 0, 0, 0 }; });
        const Img edgeDepth = Fill([](unsigned x, unsigned) { return Px { x < 16 ? 0.1f : 0.6f, 0, 0, 0 }; });
        c = Base(DlssNrEditCache_Ring);
        c.RingSlice = 0;
        c.DepthWidth = W;
        c.DepthHeight = H;
        c.DepthInverted = 1;
        gpu.Run(c, { &edgeMotion, &edgeDepth }, ring, scratch, W, H);
        expect(Near(ring.at(15, 4).r, -1.0f / W, 1e-7f), "ring: at an edge the nearer surface's vector is taken");
        expect(Near(ring.at(10, 4).r, -3.0f / W, 1e-7f), "ring: away from the edge each surface keeps its own");
    }

    // 2. Capture, then 3./4. Carry, still and panning. Frames 0 (anchor) .. 3; the background pans 1 px a frame.
    Img anchorEdit = Blank(W, H), anchorPrint = Blank(W, H);
    Img context = Blank((W + 3) / 4, (H + 3) / 4);
    {
        const Img frame = Frame(0), answer = Answer(0);
        auto c = Base(DlssNrEditCache_Context);
        gpu.Run(c, { &frame }, context, scratch, context.w, context.h);
        c = Base(DlssNrEditCache_Capture);
        gpu.Run(c, { &frame, &answer, &frame, &frame, &context }, anchorEdit, anchorPrint, W, H);
        float worst = 0.0f;
        for (unsigned y = 0; y < H; ++y)
            for (unsigned x = 0; x < W; ++x)
            {
                const Px e = EditAt((float) x, y), got = anchorEdit.at(x, y);
                worst = std::max({ worst, std::abs(e.r - got.r), std::abs(e.g - got.g), std::abs(e.b - got.b) });
            }
        expect(worst < 1e-4f, "capture: the edit is the log ratio of answer to frame (worst " + std::to_string(worst) + ")");
        const Px f = frame.at(5, 5), p = anchorPrint.at(5, 5);
        const float eps = 1.0f / 512.0f, y = Luma({ f.r + eps, f.g + eps, f.b + eps, 0 });
        expect(Near(p.r, std::log2(y), 1e-4f) && Near(p.g, std::log2((f.r + eps) / y), 1e-4f),
               "capture: the fingerprint is log luma and log chroma");
    }

    Img ring = Blank(W, H * kDlssNrEditCacheRingSlices);
    Img validPrev = Blank(W, H);
    {
        // Still: no motion, the edit comes back exactly and is believed everywhere
        const Img still = Fill([](unsigned, unsigned) { return Px { 0, 0, 0, 0 }; });
        auto c = Base(DlssNrEditCache_Ring);
        c.RingSlice = 1;
        Img stillRing = ring;
        gpu.Run(c, { &still, &zeroDepth }, stillRing, scratch, W, H);
        const Img frame = Frame(0);
        Img ctxNow = Blank(context.w, context.h);
        gpu.Run(Base(DlssNrEditCache_Context), { &frame }, ctxNow, scratch, ctxNow.w, ctxNow.h);
        c = Base(DlssNrEditCache_Carry);
        c.RingSlice = 1;
        c.TraceSteps = 1;
        c.FirstCarried = 1;
        Img carried = Blank(W, H), valid = Blank(W, H);
        gpu.Run(c, { &frame, &stillRing, &anchorEdit, &anchorPrint, &ctxNow, &validPrev }, carried, valid, W, H);
        bool exact = true, believed = true;
        for (unsigned i = 0; i < carried.px.size(); ++i)
        {
            exact = exact && Near(carried.px[i].r, anchorEdit.px[i].r, 1e-6f) && Near(carried.px[i].b, anchorEdit.px[i].b, 1e-6f);
            believed = believed && carried.px[i].a == 1.0f && valid.px[i].r == 1.0f;
        }
        expect(exact, "carry, still: the anchor's edit comes back exactly");
        expect(believed, "carry, still: believed everywhere");
    }
    {
        // Pan: frames 1..3 each move the background 1 px right (previous = current - 1 px: raw -1)
        const Img pan = Fill([](unsigned, unsigned) { return Px { -1.0f, 0, 0, 0 }; });
        Img carried = Blank(W, H), valid = Blank(W, H);
        for (unsigned t = 1; t <= 3; ++t)
        {
            auto c = Base(DlssNrEditCache_Ring);
            c.RingSlice = t;
            gpu.Run(c, { &pan, &zeroDepth }, ring, scratch, W, H);
            const Img frame = Frame((int) t);
            Img ctxNow = Blank(context.w, context.h);
            gpu.Run(Base(DlssNrEditCache_Context), { &frame }, ctxNow, scratch, ctxNow.w, ctxNow.h);
            c = Base(DlssNrEditCache_Carry);
            c.RingSlice = t;
            c.TraceSteps = t;
            c.FirstCarried = t == 1 ? 1u : 0u;
            gpu.Run(c, { &frame, &ring, &anchorEdit, &anchorPrint, &ctxNow, &validPrev }, carried, valid, W, H);
            validPrev = valid;
        }
        float worst = 0.0f;
        bool believed = true;
        for (unsigned y = 0; y < H; ++y)
            for (unsigned x = 6; x < W - 2; ++x) // away from the edge it pans in at (and the context's footprint)
            {
                const Px e = EditAt((float) x - 3, y), got = carried.at(x, y);
                worst = std::max({ worst, std::abs(e.r - got.r), std::abs(e.g - got.g) });
                believed = believed && got.a == 1.0f;
            }
        expect(worst < 1e-4f, "carry, pan: the edit stays on its surface over 3 frames (worst " + std::to_string(worst) + ")");
        expect(believed, "carry, pan: believed away from the edge");
        expect(carried.at(0, 4).a == 0.0f, "carry, pan: what panned in from off screen is not believed");
    }

    // 5. The fingerprint: a block that got much brighter is dropped, and stays dropped after it returns
    {
        const Img still = Fill([](unsigned, unsigned) { return Px { 0, 0, 0, 0 }; });
        Img stillRing = Blank(W, H * kDlssNrEditCacheRingSlices);
        for (unsigned s = 0; s < kDlssNrEditCacheRingSlices; ++s)
        {
            auto c = Base(DlssNrEditCache_Ring);
            c.RingSlice = s;
            gpu.Run(c, { &still, &zeroDepth }, stillRing, scratch, W, H);
        }
        Img changed = Frame(0);
        for (unsigned y = 6; y < 10; ++y)
            for (unsigned x = 20; x < 24; ++x)
                changed.at(x, y) = { 0.9f, 0.9f, 0.9f, 1 };
        Img ctxNow = Blank(context.w, context.h);
        gpu.Run(Base(DlssNrEditCache_Context), { &changed }, ctxNow, scratch, ctxNow.w, ctxNow.h);
        auto c = Base(DlssNrEditCache_Carry);
        c.RingSlice = 1;
        c.TraceSteps = 1;
        c.FirstCarried = 1;
        Img carried = Blank(W, H), valid = Blank(W, H);
        gpu.Run(c, { &changed, &stillRing, &anchorEdit, &anchorPrint, &ctxNow, &validPrev }, carried, valid, W, H);
        expect(valid.at(21, 7).r == 0.0f, "fingerprint: a pixel that got much brighter is dropped");
        expect(valid.at(5, 3).r == 1.0f, "fingerprint: an unchanged pixel far from it is kept");

        // Next frame: the colour is back, but only a refresh restores the pixel
        const Img back = Frame(0);
        gpu.Run(Base(DlssNrEditCache_Context), { &back }, ctxNow, scratch, ctxNow.w, ctxNow.h);
        c.RingSlice = 2;
        c.TraceSteps = 2;
        c.FirstCarried = 0;
        Img carried2 = Blank(W, H), valid2 = Blank(W, H);
        gpu.Run(c, { &back, &stillRing, &anchorEdit, &anchorPrint, &ctxNow, &valid }, carried2, valid2, W, H);
        expect(valid2.at(21, 7).r == 0.0f, "fingerprint: dropped stays dropped until a refresh");
        expect(valid2.at(5, 3).r == 1.0f, "fingerprint: the rest stays believed");
    }

    // 6. Apply and the surface fill
    {
        // A flat frame of two brightnesses, left 0.2 and right 0.6; edit 0.3 stop on the left, -0.5 on the right; the
        // pixel (10, 8) is dropped and must take the left's edit (same brightness), not the right's
        const Img frame = Fill([](unsigned x, unsigned) { const float v = x < 16 ? 0.2f : 0.6f; return Px { v, v, v, 1 }; });
        Img carried = Fill([](unsigned x, unsigned) { const float e = x < 16 ? 0.3f : -0.5f; return Px { e, e, e, 1 }; });
        carried.at(10, 8) = { 0, 0, 0, 0 };
        auto c = Base(DlssNrEditCache_Cells);
        Img atlasEdit = Blank(AtlasWidth(c), c.CellCountY0), atlasLuma = Blank(AtlasWidth(c), c.CellCountY0);
        for (uint32_t level = 0; level < kDlssNrEditCacheCellLevels; ++level)
        {
            c.CellLevel = level;
            gpu.Run(c, { &frame, &carried }, atlasEdit, atlasLuma, (&c.CellCountX0)[level], (&c.CellCountY0)[level]);
        }
        c = Base(DlssNrEditCache_Apply);
        Img out = Blank(W, H), shown = Blank(W, H);
        gpu.Run(c, { &frame, &carried, &atlasEdit, &atlasLuma, &frame, &ring }, out, shown, W, H);
        expect(Near(out.at(3, 3).r, 0.2f * std::exp2(0.3f), 1e-5f), "apply: a believed pixel shows frame x 2^edit");
        expect(Near(shown.at(10, 8).r, 0.3f, 0.01f),
               "apply: a dropped pixel takes its own surface's edit (" + std::to_string(shown.at(10, 8).r) + ")");

        // Nothing of similar brightness nearby: no edit
        Img lonely = carried;
        Img odd = frame;
        odd.at(10, 8) = { 3.0f, 3.0f, 3.0f, 1 };
        c = Base(DlssNrEditCache_Cells);
        for (uint32_t level = 0; level < kDlssNrEditCacheCellLevels; ++level)
        {
            c.CellLevel = level;
            gpu.Run(c, { &odd, &lonely }, atlasEdit, atlasLuma, (&c.CellCountX0)[level], (&c.CellCountY0)[level]);
        }
        c = Base(DlssNrEditCache_Apply);
        gpu.Run(c, { &odd, &lonely, &atlasEdit, &atlasLuma, &odd, &ring }, out, shown, W, H);
        expect(std::abs(shown.at(10, 8).r) < 0.01f, "apply: nothing similar near, no edit");
    }

    // 7. A display-encoded frame: a zero edit gives it back exactly
    {
        const Img frame = Fill([](unsigned x, unsigned y) { return Px { 0.05f + 0.03f * x, 0.5f, 0.01f * y, 1 }; });
        const Img zero = Fill([](unsigned, unsigned) { return Px { 0, 0, 0, 1 }; });
        auto c = Base(DlssNrEditCache_Apply);
        c.Passthrough = 1;
        Img out = Blank(W, H), shown = Blank(W, H);
        Img atlas = Blank(AtlasWidth(c), c.CellCountY0);
        gpu.Run(c, { &frame, &zero, &atlas, &atlas, &frame, &ring }, out, shown, W, H);
        float worst = 0.0f;
        for (unsigned i = 0; i < out.px.size(); ++i)
            worst = std::max({ worst, std::abs(out.px[i].r - frame.px[i].r), std::abs(out.px[i].b - frame.px[i].b) });
        expect(worst < 2e-6f, "display-encoded: a zero edit gives the frame back (worst " + std::to_string(worst) + ")");
    }

    // 8. Crossfade: one frame back through a still ring, 20% of the new edit
    {
        const Img frame = Frame(0);
        const Img newEdit = Fill([](unsigned, unsigned) { return Px { 0.5f, 0.5f, 0.5f, 1 }; });
        const Img fadeSource = Fill([](unsigned, unsigned) { return Px { -0.5f, -0.5f, -0.5f, 0 }; });
        Img stillRing = Blank(W, H * kDlssNrEditCacheRingSlices);
        auto c = Base(DlssNrEditCache_Apply);
        c.RingSlice = 3;
        c.FadeSteps = 1;
        c.FadeWeight = 0.2f;
        Img out = Blank(W, H), shown = Blank(W, H);
        Img atlas = Blank(AtlasWidth(c), c.CellCountY0);
        gpu.Run(c, { &frame, &newEdit, &atlas, &atlas, &fadeSource, &stillRing }, out, shown, W, H);
        expect(Near(shown.at(8, 8).r, -0.3f, 1e-5f), "crossfade: the fade source blended toward the new edit by FadeWeight");
    }

    // 9. Compose: three frames of -1 px are -3 px raw (scale 1), at the motion texture's allocation size
    {
        auto c = Base(DlssNrEditCache_Compose);
        c.RingSlice = 3;
        c.TraceSteps = 3;
        c.ComposeWidth = W;
        c.ComposeHeight = H;
        Img composed = Blank(W, H);
        gpu.Run(c, { &ring, &ring }, composed, scratch, W, H);
        expect(Near(composed.at(20, 8).r, -3.0f, 1e-4f) && Near(composed.at(20, 8).g, 0.0f, 1e-5f),
               "compose: the vectors back to the anchor in raw units (" + std::to_string(composed.at(20, 8).r) + ")");
    }

    if (fails != 0)
    {
        std::printf("%d check(s) failed\n", fails);
        return 1;
    }
    std::puts("PASS: ring, capture, carry (still and panning), fingerprint drop and hold, apply and surface fill, "
              "display-encoded round trip, crossfade, compose (WARP HLSL)");
    return 0;
}
catch (const std::exception& e)
{
    std::printf("ERROR: %s\n", e.what());
    return 2;
}
