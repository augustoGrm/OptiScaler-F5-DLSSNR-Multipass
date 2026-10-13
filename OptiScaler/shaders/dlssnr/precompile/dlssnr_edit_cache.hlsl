// The edit cache (dlssnr/DlssNrEditCache.h, DlssNr_EditCache.inl): the model runs one frame in N, and the frames
// between show its last answer carried along the game's vectors. Constants and the modes' bindings are in
// DlssNr_EditCacheConstants.h. The design follows the edit cache of Skynizz/optiscaler-dlss5 (edit-cache.md); this code
// is written here from those notes, and the choices that differ were measured offline (DLSS-TOOLS editcache).
//
// What is carried is the edit, never the picture: log2((answer + eps) / (frame + eps)) per channel, in light. Every
// frame multiplies its own fresh frame by it, so geometry is never moved -- only the model's verdict on how to light
// and detail it. A ratio because it survives the light changing under it, a log so averaging it is a geometric mean.
//
// Vectors point from the current pixel to where it was: previous = current + mv. Raw vectors times MvScale are pixels
// of the motion subrect, so dividing by its size gives a uv displacement of the image. Each frame's displacements go
// into one slice of a ring; a later frame reaches its anchor by stepping back through the slices, reading each frame's
// own vector at the position reached so far, and reads the anchor's edit there once.

#ifdef VK_MODE
#define EC_BINDING(n) [[vk::binding(n, 0)]]
#define EC_ANY_FORMAT [[vk::image_format("unknown")]]
#else
#define EC_BINDING(n)
#define EC_ANY_FORMAT
#endif

#ifdef VK_MODE
[[vk::binding(0, 0)]]
cbuffer Params : register(b0, space0)
#else
cbuffer Params : register(b0)
#endif
{
    uint mode;
    uint width;
    uint height;
    uint motionWidth;
    uint motionHeight;
    uint motionBaseX;
    uint motionBaseY;
    uint depthWidth;
    uint depthHeight;
    uint depthBaseX;
    uint depthBaseY;
    uint depthInverted;
    float mvScaleX;
    float mvScaleY;
    uint passthrough;
    float epsilon;
    uint ringSlice;
    uint traceSteps;
    uint fadeSteps;
    float fadeWeight;
    float tolerance;
    float contextTolerance;
    uint firstCarried;
    uint cellLevel;
    uint cellAtlasX0;
    uint cellAtlasX1;
    uint cellAtlasX2;
    uint cellCountX0;
    uint cellCountX1;
    uint cellCountX2;
    uint cellCountY0;
    uint cellCountY1;
    uint cellCountY2;
    float fillSigma;
    uint debugView;
    uint composeWidth;
    uint composeHeight;
    float age;
    uint ringWidth;
    uint ringHeight;
};

EC_BINDING(1) Texture2D<float4> t0 : register(t0);
EC_BINDING(2) Texture2D<float4> t1 : register(t1);
EC_BINDING(3) Texture2D<float4> t2 : register(t2);
EC_BINDING(4) Texture2D<float4> t3 : register(t3);
EC_BINDING(5) Texture2D<float4> t4 : register(t4);
EC_BINDING(9) Texture2D<float4> t5 : register(t5);
EC_BINDING(6) EC_ANY_FORMAT RWTexture2D<float4> u0 : register(u0);
EC_BINDING(7) EC_ANY_FORMAT RWTexture2D<float4> u1 : register(u1);
EC_BINDING(8) SamplerState gLinear : register(s0);

static const uint kRingSlices = 8;
static const uint kRingColumns = 4;
static const float3 kLuma = float3(0.2126, 0.7152, 0.0722);

// ---- colour ----

// lerp/step rather than a vector ternary: HLSL 2021 (dxc) takes only a scalar condition there
float3 SrgbToLinear(float3 c)
{
    c = max(c, 0.0);
    return lerp(pow((c + 0.055) / 1.055, 2.4), c / 12.92, step(c, 0.04045));
}

float3 LinearToSrgb(float3 c)
{
    c = max(c, 0.0);
    return lerp(1.055 * pow(c, 1.0 / 2.4) - 0.055, c * 12.92, step(c, 0.0031308));
}

// The frame's stored values -> light, and back (a display-encoded frame is decoded from sRGB; light is as it is)
float3 ToLight(float3 c) { return passthrough != 0 ? SrgbToLinear(c) : max(c, 0.0); }
float3 FromLight(float3 l) { return passthrough != 0 ? LinearToSrgb(l) : l; }

float LogLuma(float3 light) { return log2(dot(light, kLuma) + epsilon); }

// The fingerprint of a pixel: its log luma and two log chroma ratios (red and blue against the luma)
float3 Print(float3 light)
{
    const float3 c = light + epsilon;
    const float y = dot(c, kLuma);
    return float3(log2(y), log2(c.r / y), log2(c.b / y));
}

// ---- guides ----

int2 TexelAt(float2 uv, uint2 size) { return int2(min(uint2(saturate(uv) * float2(size)), size - 1)); }

float FarIsZero(float d) { return depthInverted != 0 ? d : 1.0 - d; }

bool HasDepth() { return depthWidth != 0 && depthHeight != 0; }

// Where slice `slice` starts in the ring (4 across, 2 down)
int2 SliceOrigin(uint slice) { return int2((slice % kRingColumns) * ringWidth, (slice / kRingColumns) * ringHeight); }

// This frame's displacement at uv, from ring slice `slice` (in `ring`)
float2 RingDisplacement(Texture2D<float4> ring, float2 uv, uint slice)
{
    const int2 texel = TexelAt(uv, uint2(ringWidth, ringHeight));
    return ring.Load(int3(SliceOrigin(slice) + texel, 0)).rg;
}

// From uv on the frame of ring slice `slice`, `steps` frames back: where it was then, and whether it stayed on screen
float2 Trace(Texture2D<float4> ring, float2 uv, uint slice, uint steps, out bool onScreen)
{
    onScreen = true;
    for (uint i = 0; i < steps && i < kRingSlices; ++i)
    {
        uv += RingDisplacement(ring, uv, (slice + kRingSlices - i) % kRingSlices);
        onScreen = onScreen && all(uv >= 0.0) && all(uv <= 1.0);
    }
    return uv;
}

// The surroundings' log luma at uv: four bilinear taps of the quarter-size context, a footprint of about 12 pixels
float ContextAt(Texture2D<float4> context, float2 uv)
{
    const float2 step = 4.0 / float2(width, height);
    float sum = 0.0;
    [unroll] for (int i = 0; i < 4; ++i)
    {
        const float2 o = float2((i & 1) != 0 ? 1.0 : -1.0, (i & 2) != 0 ? 1.0 : -1.0) * step;
        sum += context.SampleLevel(gLinear, saturate(uv + o), 0).r;
    }
    return sum * 0.25;
}

uint CellAtlasX(uint level) { return level == 0 ? cellAtlasX0 : level == 1 ? cellAtlasX1 : cellAtlasX2; }
uint CellCountX(uint level) { return level == 0 ? cellCountX0 : level == 1 ? cellCountX1 : cellCountX2; }
uint CellCountY(uint level) { return level == 0 ? cellCountY0 : level == 1 ? cellCountY1 : cellCountY2; }
uint CellSize(uint level) { return level == 0 ? 8u : level == 1 ? 32u : 128u; }

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (mode == 0)
    {
        // Ring: this frame's vectors as uv displacement, each texel's taken from the nearest surface of its 3x3 (the
        // largest far-is-zero depth), so an edge's vector stays with the object in front. The ring's own grid reads
        // the vectors at its uv.
        if (id.x >= ringWidth || id.y >= ringHeight)
            return;
        const float2 ringUv = (float2(id.xy) + 0.5) / float2(ringWidth, ringHeight);
        const int2 centre = TexelAt(ringUv, uint2(motionWidth, motionHeight));
        int2 pick = centre;
        if (HasDepth())
        {
            float best = -1.0;
            [unroll] for (int dy = -1; dy <= 1; ++dy)
            {
                [unroll] for (int dx = -1; dx <= 1; ++dx)
                {
                    const int2 m = clamp(centre + int2(dx, dy), int2(0, 0), int2(motionWidth, motionHeight) - 1);
                    const float2 uv = (float2(m) + 0.5) / float2(motionWidth, motionHeight);
                    const int2 d = TexelAt(uv, uint2(depthWidth, depthHeight));
                    const float depth = FarIsZero(t1.Load(int3(d + int2(depthBaseX, depthBaseY), 0)).r);
                    if (depth > best)
                    {
                        best = depth;
                        pick = m;
                    }
                }
            }
        }
        const float2 raw = t0.Load(int3(pick + int2(motionBaseX, motionBaseY), 0)).rg;
        float2 displacement = raw * float2(mvScaleX, mvScaleY) / float2(motionWidth, motionHeight);
        if (!all(isfinite(displacement)))
            displacement = 0.0;
        u0[uint2(SliceOrigin(ringSlice) + int2(id.xy))] = float4(displacement, 0.0, 0.0);
        return;
    }

    if (mode == 1)
    {
        // Context: the mean log luma of each 4x4 block
        const uint2 size = uint2((width + 3) / 4, (height + 3) / 4);
        if (any(id.xy >= size))
            return;
        float sum = 0.0;
        [unroll] for (int y = 0; y < 4; ++y)
        {
            [unroll] for (int x = 0; x < 4; ++x)
            {
                const int2 p = min(int2(id.xy * 4) + int2(x, y), int2(width, height) - 1);
                sum += LogLuma(ToLight(t0.Load(int3(p, 0)).rgb));
            }
        }
        u0[id.xy] = float4(sum / 16.0, 0.0, 0.0, 0.0);
        return;
    }

    if (mode == 4)
    {
        // Cells: one level of the surface fill's grid. Each cell averages the validated edit of its own pixels and
        // their log luma (both weighted by validity) and keeps the share of its pixels that were validated.
        const uint level = cellLevel;
        if (id.x >= CellCountX(level) || id.y >= CellCountY(level))
            return;
        const uint size = CellSize(level);
        float3 edit = 0.0;
        float luma = 0.0, weight = 0.0;
        if (level == 0)
        {
            for (uint y = 0; y < size; ++y)
            {
                for (uint x = 0; x < size; ++x)
                {
                    const uint2 p = id.xy * size + uint2(x, y);
                    if (p.x >= width || p.y >= height)
                        continue;
                    const float4 carried = t1.Load(int3(p, 0));
                    const float w = carried.a;
                    edit += carried.rgb * w;
                    luma += LogLuma(ToLight(t0.Load(int3(p, 0)).rgb)) * w;
                    weight += w;
                }
            }
        }
        else
        {
            // From level 0's cells: each holds its means and the share of its 64 pixels that were validated, so the
            // weighted sums come back exactly (and a thread reads 16 or 256 cells, not 1024 or 16384 pixels)
            const uint children = size / CellSize(0);
            const float childArea = float(CellSize(0) * CellSize(0));
            for (uint y = 0; y < children; ++y)
            {
                for (uint x = 0; x < children; ++x)
                {
                    const uint2 c = id.xy * children + uint2(x, y);
                    if (c.x >= cellCountX0 || c.y >= cellCountY0)
                        continue;
                    const float4 child = t2.Load(int3(cellAtlasX0 + c.x, c.y, 0));
                    const float w = child.a * childArea;
                    edit += child.rgb * w;
                    luma += t3.Load(int3(cellAtlasX0 + c.x, c.y, 0)).r * w;
                    weight += w;
                }
            }
        }
        const uint2 at = uint2(CellAtlasX(level) + id.x, id.y);
        const float inv = 1.0 / max(weight, 1e-6);
        u0[at] = float4(edit * inv, weight / float(size * size));
        u1[at] = float4(luma * inv, 0.0, 0.0, 0.0);
        return;
    }

    if (mode == 6)
    {
        // Compose: the vectors from this frame back to the frame the model last ran on, in the game's raw units, for
        // the model's own history on a refresh
        if (id.x >= composeWidth || id.y >= composeHeight)
            return;
        const int2 local = int2(id.xy) - int2(motionBaseX, motionBaseY);
        float2 raw = 0.0;
        if (all(local >= 0) && all(local < int2(motionWidth, motionHeight)))
        {
            const float2 size = float2(motionWidth, motionHeight);
            const float2 uv = (float2(local) + 0.5) / size;
            bool onScreen;
            const float2 back = Trace(t1, uv, ringSlice, traceSteps, onScreen);
            raw = (back - uv) * size / float2(mvScaleX, mvScaleY);
            if (!all(isfinite(raw)))
                raw = 0.0;
        }
        u0[id.xy] = float4(raw, 0.0, 0.0);
        return;
    }

    if (id.x >= width || id.y >= height)
        return;
    const float2 uv = (float2(id.xy) + 0.5) / float2(width, height);
    const float4 frame = t0.Load(int3(id.xy, 0));
    const float3 light = ToLight(frame.rgb);

    if (mode == 2)
    {
        // Capture: the model's edit on this frame, and the frame's fingerprint with its surroundings
        const float3 answer = ToLight(t1.Load(int3(id.xy, 0)).rgb);
        float3 edit = log2((answer + epsilon) / (light + epsilon));
        if (!all(isfinite(edit)))
            edit = 0.0;
        u0[id.xy] = float4(edit, 1.0);
        u1[id.xy] = float4(Print(light), ContextAt(t4, uv));
        return;
    }

    if (mode == 3)
    {
        // Carry: the anchor's edit, read once through the chained vectors, and whether it is still believed here
        bool onScreen;
        const float2 anchorUv = Trace(t1, uv, ringSlice, traceSteps, onScreen);
        const float3 edit = t2.SampleLevel(gLinear, anchorUv, 0).rgb;
        const float4 anchorPrint = t3.SampleLevel(gLinear, anchorUv, 0);

        // The anchor's fingerprint against this frame's 3x3 range (TAA-style, so aliasing and sub-pixel shifts pass)
        float3 lo = 1e30, hi = -1e30;
        [unroll] for (int dy = -1; dy <= 1; ++dy)
        {
            [unroll] for (int dx = -1; dx <= 1; ++dx)
            {
                const int2 p = clamp(int2(id.xy) + int2(dx, dy), int2(0, 0), int2(width, height) - 1);
                const float3 print = Print(ToLight(t0.Load(int3(p, 0)).rgb));
                lo = min(lo, print);
                hi = max(hi, print);
            }
        }
        const float3 outside = max(lo - anchorPrint.xyz, anchorPrint.xyz - hi);
        bool believed = max(outside.x, max(outside.y, outside.z)) <= tolerance;
        // The surroundings: light an object left behind on the ground changes them, not the pixel's own range
        believed = believed && abs(anchorPrint.w - ContextAt(t4, uv)) <= contextTolerance;

        // Only a refresh restores a dropped pixel: last frame's validity, where this pixel was, must still hold. Read
        // at its texel, not blended: a blend with a dropped neighbour would drop this one too, and the dropped area
        // would grow by a pixel every frame.
        if (firstCarried == 0)
        {
            const float2 previousUv = uv + RingDisplacement(t1, uv, ringSlice);
            believed = believed && t5.Load(int3(TexelAt(previousUv, uint2(width, height)), 0)).r > 0.999;
        }
        believed = believed && onScreen && all(isfinite(edit));

        const float valid = believed ? 1.0 : 0.0;
        u0[id.xy] = float4(believed ? edit : 0.0, valid);
        u1[id.xy] = float4(valid, 0.0, 0.0, 0.0);
        return;
    }

    if (mode == 5)
    {
        // Apply: a dropped pixel takes the low band of its own surface -- validated edit nearby of similar brightness,
        // from the finest grid that has some, fading to no edit where none is near
        const float4 carried = t1.Load(int3(id.xy, 0));
        float3 shown = carried.rgb;
        const float valid = carried.a;
        if (valid < 1.0)
        {
            const float pixelLuma = LogLuma(light);
            float3 fill = 0.0;
            float have = 0.0;
            [unroll] for (uint level = 0; level < 3; ++level)
            {
                const int2 cell = int2(id.xy / CellSize(level));
                const int2 count = int2(CellCountX(level), CellCountY(level));
                float3 sum = 0.0;
                float weight = 0.0;
                [unroll] for (int dy = -1; dy <= 1; ++dy)
                {
                    [unroll] for (int dx = -1; dx <= 1; ++dx)
                    {
                        const int2 c = clamp(cell + int2(dx, dy), int2(0, 0), count - 1);
                        const int2 at = int2(CellAtlasX(level) + c.x, c.y);
                        const float4 e = t2.Load(int3(at, 0));
                        const float y = t3.Load(int3(at, 0)).r;
                        const float d = (y - pixelLuma) / fillSigma;
                        const float w = min(e.a, 1.0) * exp(-0.5 * d * d);
                        sum += e.rgb * w;
                        weight += w;
                    }
                }
                const float found = min(weight, 1.0);
                fill += found * (1.0 - have) * (sum / max(weight, 1e-6));
                have += found * (1.0 - have);
            }
            shown = valid * shown + (1.0 - valid) * fill;
        }

        // Crossfade after a refresh: from what was shown before it, carried here, toward the new answer -- only where
        // the frame it was shown on (its log luma, kept beside it) still matches this frame's 3x3 range: on ground
        // uncovered since, the old edit belonged to something else
        if (fadeSteps > 0)
        {
            bool onScreen;
            const float2 fadeUv = Trace(t5, uv, ringSlice, fadeSteps, onScreen);
            const float4 before = t4.SampleLevel(gLinear, fadeUv, 0);
            float lo = 1e30, hi = -1e30;
            [unroll] for (int dy = -1; dy <= 1; ++dy)
            {
                [unroll] for (int dx = -1; dx <= 1; ++dx)
                {
                    const int2 p = clamp(int2(id.xy) + int2(dx, dy), int2(0, 0), int2(width, height) - 1);
                    const float y = LogLuma(ToLight(t0.Load(int3(p, 0)).rgb));
                    lo = min(lo, y);
                    hi = max(hi, y);
                }
            }
            const bool held = before.a >= lo - tolerance && before.a <= hi + tolerance;
            if (onScreen && held && all(isfinite(before)))
                shown = lerp(before.rgb, shown, fadeWeight);
        }

        // The inverse of the capture's ratio, epsilon and all: near black, where the frame is below epsilon, the model's
        // lift is kept rather than scaled away
        float3 output = max((light + epsilon) * exp2(shown) - epsilon, 0.0);
        if (debugView == 1 && valid < 1.0)
            output = lerp(output, float3(1.0, 0.1, 0.1) * max(dot(light, kLuma), 0.05), 0.6);
        else if (debugView == 2)
            output = lerp(output, float3(age, 1.0 - age, 0.2) * max(dot(light, kLuma), 0.05), 0.5);
        u0[id.xy] = float4(FromLight(output), frame.a);
        // What was shown, with this frame's log luma for a later crossfade's check
        u1[id.xy] = float4(shown, LogLuma(light));
        return;
    }
}
