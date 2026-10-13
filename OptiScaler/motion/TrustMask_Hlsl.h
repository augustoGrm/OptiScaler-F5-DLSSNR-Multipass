#pragma once

// The trust mask's compute shaders (motion/TrustMask_Dx12.cpp).
// Compiled ahead of time into native/F5LowShaderBytecode.h by tests/nr_shader_bytecode_smoke.cpp (run it with --write
// after changing anything here; without, it checks the embedded bytecode is current).

namespace TrustMaskHlsl
{
inline constexpr const char* kSource = R"HLSL(
cbuffer P : register(b0)
{
    uint2 size;          // the flow's size, which the mask and the depth proxy share
    uint2 depthSize;     // the scene depth's size
    float depthTolerance;
    float flowTolerance;
    float lumaTolerance;
    float decay;
    uint reversed;
    uint hasHistory;
    float fullPerFlow;
    float revealTolerance;
    uint depthCount;
    uint debugView;      // 0 the mask, 1 depth, 2 revealed, 3 flow consistency, 4 luma, 5 out of the picture (no memory)
    uint depthBoth;      // this frame and the one before both had depth: the depth checks have two real depths to compare
    uint sceneCutEnabled; // the scene-cut flag (CutFlag, t7) is there to be read
    uint guideSurface;    // GuideMotion: each picture pixel takes the motion of the flow texels on its own surface (t2, t3)
    uint3 padding;
};

SamplerState Linear : register(s0);
Texture2D<float>  SceneDepths[8] : register(t0);
Texture2D<float4> Flow : register(t0);
Texture2D<float4> FlowBefore : register(t1);
Texture2D<float>  DepthNow : register(t2);
Texture2D<float>  DepthBefore : register(t3);
Texture2D<float>  LumaNow : register(t4);
Texture2D<float>  LumaBefore : register(t5);
Texture2D<float>  MaskBefore : register(t6);
Texture2D<uint>   CutFlag : register(t7);
RWTexture2D<float>  OutFloat : register(u0);
RWTexture2D<float4> OutFlow : register(u0);
RWByteAddressBuffer Counter : register(u1);

static const float kSky = 5e5; // a proxy depth this large is the sky (or nothing drawn)

// A proxy for how far a surface is, from the raw depth: larger is farther. Reversed-Z puts near at 1 and a normal depth buffer
// puts it at 0; either way the distance goes as one over the nearness for anything well past the near plane, and the checks
// below only compare ratios, so the unknown near and far planes drop out.
[numthreads(8, 8, 1)]
void DepthProxy(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= size.x || id.y >= size.y)
        return;

    float2 uv = (float2(id.xy) + 0.5) / float2(size);
    int2 at = min(int2(uv * float2(depthSize)), int2(depthSize) - 1);

    // The nearest surface over all the copies: each holds only what its list had drawn, and the split differs per frame.
    float nearness = 1e-6;

    [unroll] for (int k = 0; k < 8; ++k)
        if (uint(k) < depthCount)
        {
            float d = SceneDepths[k].Load(int3(at, 0));
            nearness = max(nearness, reversed != 0 ? d : 1.0 - d);
        }

    OutFloat[id.xy] = min(1.0 / nearness, 1e6);
}

[numthreads(1, 1, 1)]
void ClearCounter()
{
    Counter.Store(0, 0);
}

groupshared uint gDistrusted;

[numthreads(8, 8, 1)]
void Trust(uint3 id : SV_DispatchThreadID, uint gi : SV_GroupIndex)
{
    if (gi == 0)
        gDistrusted = 0;
    GroupMemoryBarrierWithGroupSync();

    bool inside = id.x < size.x && id.y < size.y;
    float mask = 1.0;

    // The flow found a hard cut on this very frame: what the previous frame holds is another scene, so every pixel stays
    // distrusted (the history is not remembered into it either: the mask is one everywhere, as on a first frame).
    const bool cut = sceneCutEnabled != 0 && CutFlag.Load(int3(0, 0, 0)) != 0;

    if (inside && hasHistory != 0 && !cut)
    {
        int2 p = int2(id.xy);
        float2 flow = Flow.Load(int3(p, 0)).xy;       // to the previous frame, in picture pixels
        float2 q = float2(p) + flow / fullPerFlow;    // where this pixel was, in flow pixels
        float2 uv = (q + 0.5) / float2(size);

        float bad = 0.0;
        float badDepth = 0.0, badReveal = 0.0, badFlow = 0.0, badLuma = 0.0, badOutside = 0.0;

        if (any(q < -0.5) || any(q > float2(size) - 0.5))
        {
            badOutside = 1.0; // it came from outside the picture
        }
        else
        {
            int2 qi = clamp(int2(floor(q + 0.5)), 0, int2(size) - 1);

            float zNow = DepthNow.Load(int3(p, 0));

            // A game that jitters its picture for anti-aliasing moves a depth edge by a fraction of a pixel from frame to
            // frame, so a single pixel's depth would call every edge a disocclusion. The depth before is taken from the
            // 3x3 around the spot: the one closest to this depth for the disocclusion test, the farthest for the revealed
            // test (only when everything around it was nearer did a nearer surface really leave).
            float relativeBest = 1e9;
            float farthestHere = 0.0;

            [unroll] for (int j = -1; j <= 1; ++j)
                [unroll] for (int i = -1; i <= 1; ++i)
                {
                    float z = DepthBefore.Load(int3(clamp(qi + int2(i, j), 0, int2(size) - 1), 0));
                    relativeBest = min(relativeBest, abs(z - zNow) / zNow);

                    float here = DepthBefore.Load(int3(clamp(p + int2(i, j), 0, int2(size) - 1), 0));
                    farthestHere = max(farthestHere, here);
                }

            // Revealed: a surface much nearer than this one was at this very pixel a frame ago and has moved off it. The
            // flow cannot be relied on for this, it bleeds from the moving surface into what it uncovers, so it looks at
            // the same pixel instead of the flow's.
            if (depthBoth != 0 && farthestHere < zNow)
                badReveal = saturate(((zNow - farthestHere) / zNow - revealTolerance) / revealTolerance);

            if (depthBoth != 0 && zNow < kSky)
                badDepth = saturate((relativeBest - depthTolerance) / depthTolerance);

            // Consistency: the motion there was not this motion.
            float2 flowBefore = FlowBefore.SampleLevel(Linear, uv, 0).xy;
            float allowed = flowTolerance + 0.5 * length(flow);
            badFlow = saturate((length(flow - flowBefore) - allowed) / allowed);

            // Luma: what was there is outside what is here.
            float lo = 1e9, hi = -1e9;

            [unroll] for (int j = -1; j <= 1; ++j)
                [unroll] for (int i = -1; i <= 1; ++i)
                {
                    float l = LumaNow.Load(int3(clamp(p + int2(i, j), 0, int2(size) - 1), 0));
                    lo = min(lo, l);
                    hi = max(hi, l);
                }

            float before = LumaBefore.SampleLevel(Linear, uv, 0);
            float tolerance = lumaTolerance * max(hi, 0.05) + 2.0 / 255.0;
            float excursion = max(lo - before, before - hi) - tolerance;
            badLuma = saturate(excursion / (2.0 * tolerance));
        }

        bad = max(max(max(badDepth, badReveal), max(badFlow, badLuma)), badOutside);

        // Hysteresis: distrust that was there a frame ago fades, it does not vanish.
        float remembered = MaskBefore.SampleLevel(Linear, uv, 0);
        mask = saturate(max(bad, remembered * decay));

        // A debug view shows one check alone, as it is this frame.
        if (debugView == 1) mask = badDepth;
        else if (debugView == 2) mask = badReveal;
        else if (debugView == 3) mask = badFlow;
        else if (debugView == 4) mask = badLuma;
        else if (debugView == 5) mask = badOutside;
    }

    if (inside)
        OutFloat[id.xy] = mask;

    if (inside && hasHistory != 0 && mask >= 0.9)
        InterlockedAdd(gDistrusted, 1);

    GroupMemoryBarrierWithGroupSync();

    if (gi == 0 && gDistrusted != 0)
        Counter.InterlockedAdd(0, gDistrusted);
}

// The raw depth for the model at the picture's size (size): the nearest surface over the copies, in the convention they have.
[numthreads(8, 8, 1)]
void GuideDepth(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= size.x || id.y >= size.y)
        return;

    float2 uv = (float2(id.xy) + 0.5) / float2(size);
    int2 at = min(int2(uv * float2(depthSize)), int2(depthSize) - 1);
    float value = reversed != 0 ? 0.0 : 1.0;

    [unroll] for (int k = 0; k < 8; ++k)
        if (uint(k) < depthCount)
        {
            float d = SceneDepths[k].Load(int3(at, 0));
            value = reversed != 0 ? max(value, d) : min(value, d);
        }

    OutFloat[id.xy] = value;
}

// The flow enlarged to the picture's size; its values are already in picture pixels.
//
// A flow texel covers 2x2 picture pixels, and a plain bilinear read mixes the motion of the texels around a pixel: at
// the outline of a thing that moves differently from what is behind it, a band of pixels gets a motion between the two,
// which a frame generator draws as a see-through or haloed edge. With guideSurface each of the four texels counts by
// its bilinear weight and by how near its depth (DepthBefore, t3: this frame's depth proxy at the flow's size) is to
// this pixel's (DepthNow, t2: the guide depth at the picture's size, raw), measured against the nearest of the four, so
// the pixel takes its own surface's motion and a pixel whose surface no texel has takes the nearest surface's (joint
// bilateral upsampling).
[numthreads(8, 8, 1)]
void GuideMotion(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= size.x || id.y >= size.y)
        return;

    float2 uv = (float2(id.xy) + 0.5) / float2(size);

    if (guideSurface == 0)
    {
        OutFlow[id.xy] = float4(Flow.SampleLevel(Linear, uv, 0).xy, 0.0, 0.0);
        return;
    }

    uint flowWidth, flowHeight;
    Flow.GetDimensions(flowWidth, flowHeight);
    const int2 flowHi = int2(flowWidth, flowHeight) - 1;

    // This pixel's distance, as the depth proxy has it (larger is farther).
    const float raw = DepthNow.Load(int3(id.xy, 0));
    const float here = min(1.0 / max(reversed != 0 ? raw : 1.0 - raw, 1e-6), 1e6);

    const float2 f = uv * float2(flowWidth, flowHeight) - 0.5;
    const int2 f0 = int2(floor(f));
    const float2 t = f - float2(f0);

    float2 motion[4];
    float bilinear[4], apart[4];
    float closest = 1e30;

    [unroll] for (int k = 0; k < 4; ++k)
    {
        const int2 o = int2(k & 1, k >> 1);
        const int2 q = clamp(f0 + o, 0, flowHi);
        const float z = DepthBefore.Load(int3(q, 0));
        const float rel = abs(z - here) / max(min(z, here), 1e-6);

        motion[k] = Flow.Load(int3(q, 0)).xy;
        bilinear[k] = (o.x != 0 ? t.x : 1.0 - t.x) * (o.y != 0 ? t.y : 1.0 - t.y);
        apart[k] = rel * rel;
        closest = min(closest, apart[k]);
    }

    float2 sum = 0.0;
    float total = 0.0;

    [unroll] for (int k2 = 0; k2 < 4; ++k2)
    {
        // The small floor keeps a texel the pixel sits exactly beside (bilinear weight 0) in the running when it is the
        // only one on the pixel's surface.
        const float w = (bilinear[k2] + 1e-3) * exp(-(apart[k2] - closest) / 0.01);
        sum += w * motion[k2];
        total += w;
    }

    OutFlow[id.xy] = float4(sum / total, 0.0, 0.0);
}

[numthreads(8, 8, 1)]
void CopyFlow(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= size.x || id.y >= size.y)
        return;

    OutFlow[id.xy] = Flow.Load(int3(id.xy, 0));
}
)HLSL";
} // namespace TrustMaskHlsl
