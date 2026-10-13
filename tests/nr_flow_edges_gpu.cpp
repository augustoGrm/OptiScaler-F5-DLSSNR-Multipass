// GPU test for the motion Optical F5Low hands on, at object edges, no game. A textured disc (a head, a character) moves
// one way over a textured background that pans another way (the camera), each with its own depth, run through the real
// OpticalFlowDx12, TrustMaskDx12 and TrustMaskDx12::BuildGuides as the native producer calls them. Scored on the
// picture-size motion guide, which is what the model, Reuse detail and, on the native path, frame generation get, and on
// the half-resolution flow it is made from (to tell the match's errors from the enlarging's):
//   fore     inside the disc, within 4 pixels of its outline: should carry the disc's motion
//   back     outside, within 4 pixels, where the background was also to be seen last frame: the background's motion
//   reveal   outside, background that was behind the disc last frame (the disc moved off it, or it moved out from
//            under the disc): no match can find it, but its motion is the background's, which is what a frame
//            generator needs there (the background is what fills it)
//   inner    inside, more than 8 pixels in; far: outside, more than 16 pixels out
// Each is the share of pixels within 1 pixel of the truth, and the mean error in pixels. "steady": the share of outline
// pixels (fore and back) whose guide changed by at most 1 pixel from the frame before (the truth does not change).
//
//   vcvars64, then from the repo root:
//   cl /std:c++20 /EHsc /O2 tests\nr_flow_edges_gpu.cpp OptiScaler\motion\OpticalFlow_Dx12.cpp
//   OptiScaler\motion\SceneCut_Dx12.cpp OptiScaler\motion\TrustMask_Dx12.cpp d3d12.lib dxgi.lib d3dcompiler.lib
//
// Modes: no argument is the check: every case under the default settings, with and without depth, a normal depth
// buffer, a jittered depth and plain enlarging, held to limits (exit 1 on a miss). Settings as key=value (see main) run
// only those, with and without depth, and print the tallies. "perf" times BuildGuides at 2560x1440 on the GPU.

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include "../OptiScaler/motion/OpticalFlow_Dx12.h"
#include "../OptiScaler/motion/TrustMask_Dx12.h"

using Microsoft::WRL::ComPtr;

namespace
{

uint32_t kWidth = 1280;
uint32_t kHeight = 720;

float Hash(int x, int y, int seed)
{
    uint32_t h = (uint32_t) x * 374761393u + (uint32_t) y * 668265263u + (uint32_t) seed * 2246822519u;
    h = (h ^ (h >> 13)) * 1274126177u;
    h ^= h >> 16;
    return (h & 0xFFFFFF) / 16777215.0f;
}

float ValueNoise(float x, float y, float scale, int seed)
{
    const float fx = x / scale, fy = y / scale;
    const int ix = (int) std::floor(fx), iy = (int) std::floor(fy);
    const float tx = fx - ix, ty = fy - iy;
    const float sx = tx * tx * (3 - 2 * tx), sy = ty * ty * (3 - 2 * ty);
    const float a = Hash(ix, iy, seed), b = Hash(ix + 1, iy, seed), c = Hash(ix, iy + 1, seed),
                d = Hash(ix + 1, iy + 1, seed);
    return (a + (b - a) * sx) + ((c + (d - c) * sx) - (a + (b - a) * sx)) * sy;
}

float Texture(float x, float y, int channel, int seed)
{
    return 0.45f * ValueNoise(x, y, 64, seed + channel) + 0.25f * ValueNoise(x, y, 24, seed + 20 + channel) +
           0.18f * ValueNoise(x, y, 9, seed + 40 + channel) + 0.12f * ValueNoise(x, y, 4, seed + 60 + channel);
}

float Half(uint16_t h)
{
    const uint32_t sign = (h >> 15) & 1, exp = (h >> 10) & 31, mant = h & 1023;
    float v;
    if (exp == 0)
        v = std::ldexp((float) mant, -24);
    else if (exp == 31)
        v = INFINITY;
    else
        v = std::ldexp((float) (mant + 1024), (int) exp - 25);
    return sign ? -v : v;
}

struct Gpu
{
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> fence;
    HANDLE event = nullptr;
    UINT64 fenceValue = 0;

    bool Init()
    {
        ComPtr<IDXGIFactory4> factory;
        if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))
            return false;

        ComPtr<IDXGIAdapter1> adapter;
        for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i)
        {
            DXGI_ADAPTER_DESC1 desc;
            adapter->GetDesc1(&desc);
            if ((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0 &&
                SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device))))
            {
                wprintf(L"adapter: %s\n", desc.Description);
                break;
            }
        }

        if (!device)
            return false;

        D3D12_COMMAND_QUEUE_DESC qd {};
        qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        if (FAILED(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue))) ||
            FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) ||
            FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                                             IID_PPV_ARGS(&list))) ||
            FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))))
            return false;

        event = CreateEvent(nullptr, FALSE, FALSE, nullptr);
        return true;
    }

    void Submit()
    {
        list->Close();
        ID3D12CommandList* lists[] = { list.Get() };
        queue->ExecuteCommandLists(1, lists);
        queue->Signal(fence.Get(), ++fenceValue);
        fence->SetEventOnCompletion(fenceValue, event);
        WaitForSingleObject(event, INFINITE);
        allocator->Reset();
        list->Reset(allocator.Get(), nullptr);
    }

    ComPtr<ID3D12Resource> Buffer(UINT64 size, D3D12_HEAP_TYPE type, D3D12_RESOURCE_STATES state)
    {
        D3D12_HEAP_PROPERTIES heap {};
        heap.Type = type;
        D3D12_RESOURCE_DESC desc {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = size;
        desc.Height = desc.DepthOrArraySize = desc.MipLevels = 1;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ComPtr<ID3D12Resource> r;
        device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr, IID_PPV_ARGS(&r));
        return r;
    }

    // A texture filled by fill(x, y, pixel), recorded onto the list (no submit), left in the non-pixel shader resource
    // state. The upload buffer is kept in `keep` until the list has run.
    ComPtr<ID3D12Resource> Upload(DXGI_FORMAT format, uint32_t bytesPerPixel,
                                  const std::function<void(uint32_t, uint32_t, uint8_t*)>& fill,
                                  std::vector<ComPtr<ID3D12Resource>>& keep)
    {
        D3D12_HEAP_PROPERTIES heap {};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC desc {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = kWidth;
        desc.Height = kHeight;
        desc.DepthOrArraySize = desc.MipLevels = 1;
        desc.Format = format;
        desc.SampleDesc.Count = 1;
        ComPtr<ID3D12Resource> tex;
        device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                        IID_PPV_ARGS(&tex));

        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp {};
        UINT64 total = 0;
        device->GetCopyableFootprints(&desc, 0, 1, 0, &fp, nullptr, nullptr, &total);
        auto upload = Buffer(total, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);

        uint8_t* data = nullptr;
        upload->Map(0, nullptr, (void**) &data);
        for (uint32_t y = 0; y < kHeight; ++y)
            for (uint32_t x = 0; x < kWidth; ++x)
                fill(x, y, data + (size_t) y * fp.Footprint.RowPitch + (size_t) x * bytesPerPixel);
        upload->Unmap(0, nullptr);

        D3D12_TEXTURE_COPY_LOCATION dst {}, src {};
        dst.pResource = tex.Get();
        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.pResource = upload.Get();
        src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src.PlacedFootprint = fp;
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

        D3D12_RESOURCE_BARRIER b {};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = tex.Get();
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        list->ResourceBarrier(1, &b);
        keep.push_back(upload);
        return tex;
    }

    // An RGBA16F texture in the given resting state as x, y pairs, row by row.
    std::vector<float> ReadMotion(ID3D12Resource* tex, D3D12_RESOURCE_STATES rest)
    {
        const auto desc = tex->GetDesc();
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp {};
        UINT64 total = 0;
        device->GetCopyableFootprints(&desc, 0, 1, 0, &fp, nullptr, nullptr, &total);
        auto readback = Buffer(total, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);

        D3D12_RESOURCE_BARRIER b {};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = tex;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = rest;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        list->ResourceBarrier(1, &b);

        D3D12_TEXTURE_COPY_LOCATION dst {}, src {};
        dst.pResource = readback.Get();
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint = fp;
        src.pResource = tex;
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

        std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
        list->ResourceBarrier(1, &b);
        Submit();

        std::vector<float> out((size_t) desc.Width * desc.Height * 2);
        uint8_t* data = nullptr;
        readback->Map(0, nullptr, (void**) &data);
        for (uint32_t y = 0; y < desc.Height; ++y)
            for (uint32_t x = 0; x < desc.Width; ++x)
            {
                const uint16_t* px = (const uint16_t*) (data + (size_t) y * fp.Footprint.RowPitch + x * 8);
                out[((size_t) y * desc.Width + x) * 2] = Half(px[0]);
                out[((size_t) y * desc.Width + x) * 2 + 1] = Half(px[1]);
            }
        readback->Unmap(0, nullptr);
        return out;
    }
};

// One case: the background pans by camera per frame, the disc moves by object per frame (both in picture pixels, the
// way the content moves on screen). look: the disc differs from the background in brightness, as a character usually
// does; without it the two have the same texture statistics (the hardest case).
struct Case
{
    const char* name;
    float cameraX, cameraY;
    float objectX, objectY;
    float radius;
    bool look;
};

constexpr int kFrames = 10; // frames run; the last is scored (the flow's history and still age have settled)

struct Disc
{
    float cx, cy, r;
};

Disc DiscAt(const Case& c, int frame)
{
    // Starts where it ends up near the middle of the picture after the frames run.
    const float endX = 0.5f * kWidth, endY = 0.5f * kHeight;
    return { endX - c.objectX * (kFrames - 1 - frame), endY - c.objectY * (kFrames - 1 - frame), c.radius };
}

bool Inside(const Disc& d, float x, float y) { return std::hypot(x - d.cx, y - d.cy) < d.r; }

struct Tally
{
    double within = 0, error = 0;
    uint64_t n = 0;

    void Add(float ex, float ey)
    {
        const double e = std::hypot(ex, ey);
        within += e <= 1.0;
        error += e;
        ++n;
    }
    double Share() const { return n ? within / n : 0.0; }
    double Mean() const { return n ? error / n : 0.0; }
};

enum Band
{
    kFore,
    kBack,
    kReveal,
    kInner,
    kFar,
    kBands
};
const char* kBandNames[kBands] = { "fore", "back", "reveal", "inner", "far" };

// Which band a point (picture pixels) is in on `frame`, or -1.
int BandOf(const Case& c, float x, float y, int frame = kFrames - 1)
{
    const Disc now = DiscAt(c, frame), before = DiscAt(c, frame - 1);
    const float r = std::hypot(x - now.cx, y - now.cy);
    if (r < now.r)
        return now.r - r <= 4.0f ? kFore : now.r - r > 8.0f ? kInner : -1;
    // Where this background was a frame ago (it moved with the camera) lay under the disc: it has just come out from
    // behind it, whether the disc moved off it or the background moved out from under it.
    if (Inside(before, x - c.cameraX, y - c.cameraY))
        return kReveal;
    return r - now.r <= 4.0f ? kBack : r - now.r > 16.0f ? kFar : -1;
}

struct Settings
{
    bool depth = true;
    int smooth = -1;  // the flow's smoothing radius (-1: its default)
    int surface = -1; // the guide's enlarging by surface (-1: its default)
    int uncover = -1; // uncovered background given its surface's motion (-1: its default)
    float reach = -1; // ... gathered from this far (-1: its default)
    float jitter = 0; // the depth's disc is this many pixels off the colour's, alternating left and right per frame
                      // (a game that jitters its picture for anti-aliasing)
    bool reversed = true; // reversed-Z depth (near is 1); false: a normal depth buffer (near is 0)

    // The guide is the flow's plain bilinear enlarging: no depth, or both depth steps off.
    bool Bilinear() const { return !depth || (surface == 0 && uncover == 0); }
};

void Apply(const Settings& s, OpticalFlowDx12& flow, TrustMaskDx12& trust)
{
    if (s.smooth >= 0)
        flow.Tuning().smoothRadius = s.smooth;
    if (s.surface >= 0)
        trust.Tuning().guideSurface = s.surface != 0;
    if (s.uncover >= 0)
        trust.Tuning().uncoverBackground = s.uncover != 0;
    if (s.reach >= 0)
        trust.Tuning().uncoverReach = s.reach;
}

struct Result
{
    Tally guide[kBands];
    Tally flow[kBands];
    Tally steady;            // outline pixels (fore and back, the same band both frames): the guide's change from the
                             // frame before, whose truth is none (a jittered depth must not make the outline flicker)
    double bilinearOff = -1; // largest distance (pixels) of the guide from a bilinear enlarging of the flow, when the
                             // guide should be exactly that (Settings::Bilinear); -1: not checked
};

// The flow (fw x fh, x y pairs) enlarged to the picture by a clamp-to-edge bilinear read at pixel centres.
void BilinearAt(const std::vector<float>& flow, uint32_t fw, uint32_t fh, uint32_t x, uint32_t y, float* out)
{
    const float fx = ((float) x + 0.5f) / kWidth * fw - 0.5f, fy = ((float) y + 0.5f) / kHeight * fh - 0.5f;
    const int x0 = (int) std::floor(fx), y0 = (int) std::floor(fy);
    const float tx = fx - x0, ty = fy - y0;
    auto at = [&](int ix, int iy, int ch)
    {
        ix = std::clamp(ix, 0, (int) fw - 1);
        iy = std::clamp(iy, 0, (int) fh - 1);
        return flow[((size_t) iy * fw + ix) * 2 + ch];
    };
    for (int ch = 0; ch < 2; ++ch)
        out[ch] = (at(x0, y0, ch) * (1 - tx) + at(x0 + 1, y0, ch) * tx) * (1 - ty) +
                  (at(x0, y0 + 1, ch) * (1 - tx) + at(x0 + 1, y0 + 1, ch) * tx) * ty;
}

Result Run(Gpu& gpu, const Case& c, const Settings& settings)
{
    Result result;
    OpticalFlowDx12 flow;
    TrustMaskDx12 trust;

    if (!flow.Init(gpu.device.Get()) || !trust.Init(gpu.device.Get()))
    {
        printf("init failed: %s %s\n", flow.Error().c_str(), trust.Error().c_str());
        exit(1);
    }
    Apply(settings, flow, trust);
    std::vector<float> previousGuide;

    for (int frame = 0; frame < kFrames; ++frame)
    {
        const Disc disc = DiscAt(c, frame);
        const float panX = c.cameraX * frame, panY = c.cameraY * frame;
        std::vector<ComPtr<ID3D12Resource>> keep;

        auto colour = gpu.Upload(DXGI_FORMAT_R8G8B8A8_UNORM, 4,
                                 [&](uint32_t x, uint32_t y, uint8_t* px)
                                 {
                                     const bool in = Inside(disc, (float) x + 0.5f, (float) y + 0.5f);
                                     for (int ch = 0; ch < 3; ++ch)
                                     {
                                         float v = in ? Texture(x - disc.cx + 3000.0f, y - disc.cy + 3000.0f, ch, 507)
                                                      : Texture(x - panX, y - panY, ch, 7);
                                         if (in && c.look)
                                             v = 0.25f + 0.6f * v;
                                         px[ch] = (uint8_t) std::clamp(v * 255.0f + 0.5f, 0.0f, 255.0f);
                                     }
                                     px[3] = 255;
                                 },
                                 keep);

        ComPtr<ID3D12Resource> depth;
        if (settings.depth)
            depth = gpu.Upload(DXGI_FORMAT_R32_FLOAT, 4,
                               [&](uint32_t x, uint32_t y, uint8_t* px)
                               {
                                   const float shift = (frame & 1) != 0 ? settings.jitter : -settings.jitter;
                                   const float nearness =
                                       0.1f / (Inside(disc, (float) x + 0.5f - shift, (float) y + 0.5f) ? 5.0f : 20.0f);
                                   const float d = settings.reversed ? nearness : 1.0f - nearness;
                                   memcpy(px, &d, 4);
                               },
                               keep);

        flow.Dispatch(gpu.list.Get(), colour.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, depth.Get(),
                      settings.depth ? DXGI_FORMAT_R32_FLOAT : DXGI_FORMAT_UNKNOWN, settings.reversed);

        bool built = false;
        if (flow.FlowValid())
        {
            TrustMaskDx12::Inputs in;
            in.flow = flow.Flow();
            in.flowWidth = flow.FlowWidth();
            in.flowHeight = flow.FlowHeight();
            in.fullPerFlow = (float) kWidth / (float) flow.FlowWidth();
            in.lumaNow = flow.LumaOfLastFrame();
            in.lumaBefore = flow.LumaOfFrameBefore();
            in.sceneCut = flow.SceneCutFlag();

            if (settings.depth)
            {
                in.depths[0] = depth.Get();
                in.depthCount = 1;
                in.depthFormat = DXGI_FORMAT_R32_FLOAT;
                in.depthWidth = kWidth;
                in.depthHeight = kHeight;
                in.depthReversed = settings.reversed;
            }

            if (trust.Dispatch(gpu.list.Get(), in))
                built = trust.BuildGuides(gpu.list.Get(), in, kWidth, kHeight);
        }

        gpu.Submit();

        if (frame < kFrames - 2)
            continue;

        if (!built)
        {
            printf("%s: no guide on frame %d\n", c.name, frame);
            exit(1);
        }

        const auto guide = gpu.ReadMotion(trust.GuideMotion(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

        if (frame == kFrames - 2)
        {
            previousGuide = guide;
            continue;
        }

        // The flow points from here to where the content was: minus its motion.
        const float bgX = -c.cameraX, bgY = -c.cameraY, fgX = -c.objectX, fgY = -c.objectY;

        for (uint32_t y = 0; y < kHeight; ++y)
            for (uint32_t x = 0; x < kWidth; ++x)
            {
                const int band = BandOf(c, x + 0.5f, y + 0.5f);
                if ((band != kFore && band != kBack) || BandOf(c, x + 0.5f, y + 0.5f, kFrames - 2) != band)
                    continue;
                const size_t i = ((size_t) y * kWidth + x) * 2;
                result.steady.Add(guide[i] - previousGuide[i], guide[i + 1] - previousGuide[i + 1]);
            }
        for (uint32_t y = 0; y < kHeight; ++y)
            for (uint32_t x = 0; x < kWidth; ++x)
            {
                const int band = BandOf(c, x + 0.5f, y + 0.5f);
                if (band < 0)
                    continue;
                const bool fg = band == kFore || band == kInner;
                const size_t i = ((size_t) y * kWidth + x) * 2;
                result.guide[band].Add(guide[i] - (fg ? fgX : bgX), guide[i + 1] - (fg ? fgY : bgY));
            }

        const auto half = gpu.ReadMotion(flow.Flow(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
                                                         D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        const uint32_t fw = flow.FlowWidth(), fh = flow.FlowHeight();
        const float per = (float) kWidth / fw;

        if (settings.Bilinear())
        {
            result.bilinearOff = 0;
            for (uint32_t y = 0; y < kHeight; ++y)
                for (uint32_t x = 0; x < kWidth; ++x)
                {
                    float b[2];
                    BilinearAt(half, fw, fh, x, y, b);
                    const size_t i = ((size_t) y * kWidth + x) * 2;
                    result.bilinearOff =
                        std::max(result.bilinearOff, (double) std::hypot(guide[i] - b[0], guide[i + 1] - b[1]));
                }
        }

        // EDGE_ROW=1: the flow's x along the row through the disc's centre, across its right edge (for a look).
        if (getenv("EDGE_ROW") != nullptr)
        {
            const Disc d = DiscAt(c, kFrames - 1);
            const int row = (int) (d.cy / per), edge = (int) ((d.cx + d.r) / per);
            printf("  %s row %d, flow texels from the edge-8 (edge at %d):", c.name, row, edge);
            for (int x = edge - 8; x <= edge + 8; ++x)
                printf(" %.1f", half[((size_t) row * fw + x) * 2]);
            printf("\n");
        }
        for (uint32_t y = 0; y < fh; ++y)
            for (uint32_t x = 0; x < fw; ++x)
            {
                const int band = BandOf(c, (x + 0.5f) * per, (y + 0.5f) * per);
                if (band < 0)
                    continue;
                const bool fg = band == kFore || band == kInner;
                const size_t i = ((size_t) y * fw + x) * 2;
                result.flow[band].Add(half[i] - (fg ? fgX : bgX), half[i + 1] - (fg ? fgY : bgY));
            }
    }

    return result;
}

// GPU time of BuildGuides at the current size, median of several frames. busy: the picture is also covered with small
// near discs 40 pixels apart (foliage, a fence): nearly every pixel has a nearer surface around, the uncover pass's
// worst case.
double TimeGuides(Gpu& gpu, const Settings& settings, bool busy)
{
    OpticalFlowDx12 flow;
    TrustMaskDx12 trust;
    flow.Init(gpu.device.Get());
    trust.Init(gpu.device.Get());
    Apply(settings, flow, trust);

    ComPtr<ID3D12QueryHeap> heap;
    D3D12_QUERY_HEAP_DESC qd {};
    qd.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    qd.Count = 2;
    gpu.device->CreateQueryHeap(&qd, IID_PPV_ARGS(&heap));
    auto readback = gpu.Buffer(16, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
    UINT64 frequency = 1;
    gpu.queue->GetTimestampFrequency(&frequency);

    const Case c { "perf", 6, 0, -10, 4, 200, true };
    std::vector<double> times;

    for (int frame = 0; frame < 24; ++frame)
    {
        const Disc disc = DiscAt(c, std::min(frame, kFrames - 1));
        std::vector<ComPtr<ID3D12Resource>> keep;
        auto colour = gpu.Upload(DXGI_FORMAT_R8G8B8A8_UNORM, 4,
                                 [&](uint32_t x, uint32_t y, uint8_t* px)
                                 {
                                     const bool in = Inside(disc, (float) x, (float) y);
                                     const uint8_t v = (uint8_t) (255.0f * Hash((int) x / 3 + frame * 2, (int) y / 3, in ? 9 : 3));
                                     px[0] = px[1] = px[2] = v;
                                     px[3] = 255;
                                 },
                                 keep);
        auto depth = gpu.Upload(DXGI_FORMAT_R32_FLOAT, 4,
                                [&](uint32_t x, uint32_t y, uint8_t* px)
                                {
                                    const float gx = (float) ((x + 3 * frame) % 40) - 20.0f, gy = (float) (y % 40) - 20.0f;
                                    const bool leaf = busy && gx * gx + gy * gy < 144.0f;
                                    const float d =
                                        0.1f / (Inside(disc, (float) x, (float) y) ? 5.0f : leaf ? 8.0f : 20.0f);
                                    memcpy(px, &d, 4);
                                },
                                keep);

        flow.Dispatch(gpu.list.Get(), colour.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, settings.depth ? depth.Get() : nullptr,
                      settings.depth ? DXGI_FORMAT_R32_FLOAT : DXGI_FORMAT_UNKNOWN, true);

        if (flow.FlowValid())
        {
            TrustMaskDx12::Inputs in;
            in.flow = flow.Flow();
            in.flowWidth = flow.FlowWidth();
            in.flowHeight = flow.FlowHeight();
            in.fullPerFlow = (float) kWidth / (float) flow.FlowWidth();
            in.lumaNow = flow.LumaOfLastFrame();
            in.lumaBefore = flow.LumaOfFrameBefore();
            in.sceneCut = flow.SceneCutFlag();
            if (settings.depth)
            {
                in.depths[0] = depth.Get();
                in.depthCount = 1;
                in.depthFormat = DXGI_FORMAT_R32_FLOAT;
                in.depthWidth = kWidth;
                in.depthHeight = kHeight;
            }
            trust.Dispatch(gpu.list.Get(), in);
            gpu.list->EndQuery(heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0);
            trust.BuildGuides(gpu.list.Get(), in, kWidth, kHeight);
            gpu.list->EndQuery(heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 1);
            gpu.list->ResolveQueryData(heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, 2, readback.Get(), 0);
        }

        gpu.Submit();

        if (flow.FlowValid() && frame >= 4)
        {
            UINT64* t = nullptr;
            readback->Map(0, nullptr, (void**) &t);
            times.push_back((double) (t[1] - t[0]) * 1000.0 / (double) frequency);
            readback->Unmap(0, nullptr);
        }
    }

    std::sort(times.begin(), times.end());
    return times.empty() ? 0.0 : times[times.size() / 2];
}

} // namespace

int main(int argc, char** argv)
{
    Gpu gpu;
    if (!gpu.Init())
    {
        printf("no D3D12 device\n");
        return 2;
    }

    bool perf = false;
    Settings settings;
    bool onlyDepth = false, onlyNoDepth = false;

    for (int a = 1; a < argc; ++a)
    {
        const std::string arg = argv[a];
        const auto eq = arg.find('=');
        const std::string key = arg.substr(0, eq);
        const float value = eq == std::string::npos ? 0.0f : (float) atof(arg.c_str() + eq + 1);

        if (arg == "perf")
            perf = true;
        else if (key == "depth")
            (value != 0 ? onlyDepth : onlyNoDepth) = true;
        else if (key == "smooth")
            settings.smooth = (int) value;
        else if (key == "surface")
            settings.surface = (int) value;
        else if (key == "uncover")
            settings.uncover = (int) value;
        else if (key == "reach")
            settings.reach = value;
        else if (key == "jitter")
            settings.jitter = value;
        else
        {
            printf("unknown argument %s\n", arg.c_str());
            return 2;
        }
    }

    if (perf)
    {
        kWidth = 2560;
        kHeight = 1440;
        settings.depth = !onlyNoDepth;
        printf("BuildGuides at %ux%u (%s depth): %.4f ms, busy depth %.4f ms\n", kWidth, kHeight,
               settings.depth ? "with" : "no", TimeGuides(gpu, settings, false), TimeGuides(gpu, settings, true));
        return 0;
    }

    // The camera-turn case first: the character stays near the middle while the world behind it moves fast.
    const Case cases[] = {
        { "turn    r120", 18, 4, 0, 0, 120, true },   { "turn    r40 ", 18, 4, 0, 0, 40, true },
        { "turnFast r60", 36, 0, 2, 0, 60, true },    { "cross   r120", 6, 0, -10, 4, 120, true },
        { "cross   r40 ", 6, 0, -10, 4, 40, true },   { "walk    r60 ", 0, 0, 12, 0, 60, true },
        { "alike   r60 ", 18, 4, 0, 0, 60, false },   { "alikeX  r60 ", 6, 0, -10, 4, 60, false },
    };

    // With no argument, the check: the default settings with and without depth, a normal depth buffer, a jittered depth,
    // and both depth steps off (plain enlarging) for comparison, each held to the limits below. With settings given,
    // only those, with and without depth (depth=0/1 picks one), and no limits.
    struct Series
    {
        const char* label;
        Settings settings;
        Tally guide[kBands], flow[kBands];
        Tally steady;
        double bilinearOff = -1;
    };
    std::vector<Series> series;
    const bool check = argc == 1;

    auto add = [&](const char* label, auto change)
    {
        Series s { label, settings };
        change(s.settings);
        series.push_back(s);
    };

    if (check)
    {
        add("depth", [](Settings&) {});
        add("no depth", [](Settings& s) { s.depth = false; });
        add("depth, normal Z", [](Settings& s) { s.reversed = false; });
        add("depth, plain", [](Settings& s) { s.surface = s.uncover = 0; });
        add("depth, jitter 1", [](Settings& s) { s.jitter = 1; });
        add("depth, jitter 1, plain",
            [](Settings& s)
            {
                s.jitter = 1;
                s.surface = s.uncover = 0;
            });
    }
    else
    {
        if (!onlyNoDepth)
            add("depth", [](Settings&) {});
        if (!onlyDepth)
            add("no depth", [](Settings& s) { s.depth = false; });
    }

    for (Series& s : series)
    {
        printf("%-22s guide (picture size): share within 1 px / mean error, steady | half-res flow\n", s.label);

        for (const Case& c : cases)
        {
            const Result r = Run(gpu, c, s.settings);
            printf("  %s", c.name);
            for (int b = 0; b < kBands; ++b)
                printf("  %s %.3f/%.2f", kBandNames[b], r.guide[b].Share(), r.guide[b].Mean());
            printf("  steady %.3f", r.steady.Share());
            if (r.bilinearOff >= 0)
                printf("  bilinear off %.3f", r.bilinearOff);
            printf("  |");
            for (int b = 0; b < kBands; ++b)
                printf(" %.3f", r.flow[b].Share());
            printf("\n");

            for (int b = 0; b < kBands; ++b)
            {
                // Each case counts alike in the summary, whatever its pixel count; a band the case does not have
                // (nothing is uncovered where the disc stands still) does not count.
                if (r.guide[b].n == 0)
                    continue;
                s.guide[b].within += r.guide[b].Share();
                s.guide[b].error += r.guide[b].Mean();
                ++s.guide[b].n;
                s.flow[b].within += r.flow[b].Share();
                ++s.flow[b].n;
            }
            s.steady.within += r.steady.Share();
            ++s.steady.n;
            s.bilinearOff = std::max(s.bilinearOff, r.bilinearOff);
        }
    }

    for (const Series& s : series)
    {
        printf("SUMMARY %-22s", s.label);
        for (int b = 0; b < kBands; ++b)
            printf(" %s %.3f/%.2f", kBandNames[b], s.guide[b].Share(), s.guide[b].Mean());
        printf(" steady %.3f", s.steady.Share());
        if (s.bilinearOff >= 0)
            printf(" bilinear off %.3f", s.bilinearOff);
        printf(" | flow");
        for (int b = 0; b < kBands; ++b)
            printf(" %.3f", s.flow[b].Share());
        printf("\n");
    }

    if (!check)
        return 0;

    // The limits, a margin under what the steps reach (2026-10-13: depth fore 0.975, back 0.982, reveal 0.957).
    int fails = 0;
    auto expect = [&](bool ok, const char* what, double value, double limit)
    {
        if (!ok)
        {
            printf("FAIL %s: %.3f, limit %.3f\n", what, value, limit);
            ++fails;
        }
    };
    const Series &depth = series[0], &none = series[1], &normalZ = series[2], &plain = series[3],
                 &jitter = series[4], &jitterPlain = series[5];

    expect(depth.guide[kFore].Share() >= 0.95, "depth: inside the outline", depth.guide[kFore].Share(), 0.95);
    expect(depth.guide[kBack].Share() >= 0.95, "depth: beside the outline", depth.guide[kBack].Share(), 0.95);
    expect(depth.guide[kReveal].Share() >= 0.90, "depth: uncovered", depth.guide[kReveal].Share(), 0.90);

    // Away from the outline the steps change nothing.
    for (int b : { kInner, kFar })
        expect(std::abs(depth.guide[b].Share() - plain.guide[b].Share()) <= 0.01,
               b == kInner ? "depth: inner, against plain" : "depth: far, against plain", depth.guide[b].Share(),
               plain.guide[b].Share());

    // A normal depth buffer reads the same as reversed-Z.
    for (int b : { kFore, kBack, kReveal })
        expect(std::abs(normalZ.guide[b].Share() - depth.guide[b].Share()) <= 0.01, "normal Z against reversed-Z",
               normalZ.guide[b].Share(), depth.guide[b].Share());

    // Without depth, or with both steps off, the guide is the plain bilinear enlarging it always was. The GPU's sampler
    // weighs with a few fractional bits: between texels 34 pixels of motion apart (turnFast) that is 0.125 px off.
    expect(none.bilinearOff >= 0 && none.bilinearOff <= 0.25, "no depth: off a bilinear enlarging (px)",
           none.bilinearOff, 0.25);
    expect(plain.bilinearOff >= 0 && plain.bilinearOff <= 0.25, "both steps off: off a bilinear enlarging (px)",
           plain.bilinearOff, 0.25);

    // A depth jittered by a pixel each frame (a game's anti-aliasing): the steps still help, and the outline is no less
    // steady from frame to frame than with plain enlarging.
    expect(jitter.guide[kFore].Share() >= jitterPlain.guide[kFore].Share(), "jitter: inside, against plain",
           jitter.guide[kFore].Share(), jitterPlain.guide[kFore].Share());
    expect(jitter.guide[kBack].Share() >= jitterPlain.guide[kBack].Share(), "jitter: beside, against plain",
           jitter.guide[kBack].Share(), jitterPlain.guide[kBack].Share());
    expect(jitter.steady.Share() >= jitterPlain.steady.Share() - 0.01, "jitter: outline steadiness, against plain",
           jitter.steady.Share(), jitterPlain.steady.Share() - 0.01);

    printf(fails == 0 ? "PASS\n" : "FAILED (%d)\n", fails);
    return fails == 0 ? 0 : 1;
}
