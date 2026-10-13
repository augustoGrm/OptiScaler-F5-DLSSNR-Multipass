// The edit cache, D3D12 (menu: Reuse detail -> Edit cache; ini EditCache*).
//
// The model runs one frame in N (a refresh) and the frames between (cached) show its last answer carried along the
// game's vectors: frame x 2^edit, the edit read from the refresh frame (the anchor) through the vectors chained back
// frame by frame. The cadence and when it stands aside are dlssnr/DlssNrEditCache.h, the GPU work
// precompile/dlssnr_edit_cache.hlsl. After the edit cache design of Skynizz/optiscaler-dlss5 (edit-cache.md), written
// here from those notes.
//
// Included inside DlssNr_Dx12.cpp's anonymous namespace, after CreateScratch / ParkNrResource / Barrier. Dispatch calls,
// under g_nrMutex: BeforeModel after the encode (every frame: the ring; a refresh: the model's composed vectors; a
// cached frame: everything up to the output), ApplyCached in place of the resolve on a cached frame, and AfterResolve
// on a refresh (the anchor, and the crossfade's first frame). Its textures exist only while it runs.
namespace EditCache
{
// The pass's input slots (t0-t5): DispatchEditCache takes exactly that many, so a wrong count does not compile
constexpr unsigned int kInputs = 6;

struct Frame
{
    DlssNr_Dx12* pass = nullptr;
    ID3D12GraphicsCommandList* cmdList = nullptr;
    ID3D12Device* device = nullptr;
    const Config* cfg = nullptr;
    DlssNrEditCache::Inputs inputs;
    unsigned int motionBaseX = 0, motionBaseY = 0, motionAllocWidth = 0, motionAllocHeight = 0;
    unsigned int depthWidth = 0, depthHeight = 0, depthBaseX = 0, depthBaseY = 0;
    bool depthInverted = false;
    float mvScaleX = 1.0f, mvScaleY = 1.0f;
    bool passthrough = false;
    float white = 1.0f;
    ID3D12Resource* frame = nullptr;  // the untouched frame (hdrCopy), NON_PIXEL_SHADER_RESOURCE
    ID3D12Resource* motion = nullptr; // the guides as the pass reads them, NON_PIXEL_SHADER_RESOURCE
    ID3D12Resource* depth = nullptr;
};

struct Plan
{
    bool runs = false;
    bool refresh = false;
    bool cached = false;      // skip the model and the resolve; ApplyCached writes the output
    bool resetModel = false;  // the model's history is too old and no vectors reach back to it
    ID3D12Resource* motion = nullptr; // the vectors the model evaluates with, and their subrect origin
    unsigned int motionBaseX = 0, motionBaseY = 0;
};

// A texture of ours and the state it is in. They rest in UNORDERED_ACCESS.
struct Tex
{
    ID3D12Resource* r = nullptr;
    D3D12_RESOURCE_STATES s = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

    void To(ID3D12GraphicsCommandList* cmd, D3D12_RESOURCE_STATES to)
    {
        if (r != nullptr && s != to)
        {
            Barrier(cmd, r, s, to);
            s = to;
        }
    }
    void Read(ID3D12GraphicsCommandList* cmd) { To(cmd, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE); }
    void Write(ID3D12GraphicsCommandList* cmd) { To(cmd, D3D12_RESOURCE_STATE_UNORDERED_ACCESS); }
    void Park()
    {
        ParkNrResource(r);
        s = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    }
};

DlssNrEditCache::Cadence cadence;
DlssNrEditCache::Step step;
// level0Edit / level0Luma: a copy of the cell atlases after level 0, which the coarser levels are built from
Tex ring, anchorEdit, anchorPrint, context, carried, valid[2], atlasEdit, atlasLuma, level0Edit, level0Luma, shown,
    fadeSource, composed;
unsigned int curValid = 0;
unsigned int sizeW = 0, sizeH = 0, ringW = 0, ringH = 0, composedW = 0, composedH = 0;
DlssNrEditCacheConstants params {};
bool failed = false;       // allocation failed at this size: stays off until a size changes
unsigned int failedW = 0, failedH = 0, failedRingW = 0, failedRingH = 0;
bool lastCached = false;   // the last frame skipped the model (its history is that many frames old)
float modelShare = 0.0f;   // for the menu: share of frames the model ran on, refreshed every second
std::chrono::steady_clock::time_point shareSince {};
const char* loggedAside = "";
// For the menu, which never waits for g_nrMutex: published at every evaluate
std::atomic<bool> publishedRunning { false };
std::atomic<float> publishedShare { 0.0f };
std::atomic<const char*> publishedAside { nullptr }; // a string literal of DlssNrEditCache::StandAsideReason

Tex* AllTextures[] = { &ring,      &anchorEdit, &anchorPrint, &context,    &carried, &valid[0],   &valid[1],
                       &atlasEdit, &atlasLuma,  &level0Edit,  &level0Luma, &shown,   &fadeSource, &composed };

void ParkAll()
{
    for (Tex* t : AllTextures)
        t->Park();
    sizeW = sizeH = ringW = ringH = composedW = composedH = 0;
    cadence.Drop();
}

uint32_t Cells(uint32_t size, uint32_t level) { return (size + kDlssNrEditCacheCellSize[level] - 1) / kDlssNrEditCacheCellSize[level]; }

// The cell levels' fields of the constants, by level (separate members, so not indexed as an array)
void SetLevel(DlssNrEditCacheConstants& c, uint32_t level, uint32_t atlasX, uint32_t countX, uint32_t countY)
{
    uint32_t* const x[] = { &c.CellAtlasX0, &c.CellAtlasX1, &c.CellAtlasX2 };
    uint32_t* const w[] = { &c.CellCountX0, &c.CellCountX1, &c.CellCountX2 };
    uint32_t* const h[] = { &c.CellCountY0, &c.CellCountY1, &c.CellCountY2 };
    *x[level] = atlasX;
    *w[level] = countX;
    *h[level] = countY;
}

uint32_t LevelCountX(const DlssNrEditCacheConstants& c, uint32_t level)
{
    return level == 0 ? c.CellCountX0 : level == 1 ? c.CellCountX1 : c.CellCountX2;
}

uint32_t LevelCountY(const DlssNrEditCacheConstants& c, uint32_t level)
{
    return level == 0 ? c.CellCountY0 : level == 1 ? c.CellCountY1 : c.CellCountY2;
}

// One slice of the ring: the motion texture's allocation (the subrect moves inside it as the render size changes, so a
// dynamic-resolution game does not rebuild), grown only.
unsigned int RingWidthFor(const Frame& f) { return std::max({ f.motionAllocWidth, f.inputs.motionWidth, ringW }); }
unsigned int RingHeightFor(const Frame& f) { return std::max({ f.motionAllocHeight, f.inputs.motionHeight, ringH }); }

// Whether the textures fit this frame; if not, they are made anew (and the cadence starts over, before it plans the
// frame: see BeforeModel).
bool Allocate(const Frame& f)
{
    const auto& in = f.inputs;
    const unsigned int wantComposedW = std::max(f.motionAllocWidth, in.motionWidth);
    const unsigned int wantComposedH = std::max(f.motionAllocHeight, in.motionHeight);
    const unsigned int wantRingW = RingWidthFor(f);
    const unsigned int wantRingH = RingHeightFor(f);
    if (sizeW == in.width && sizeH == in.height && ringW == wantRingW && ringH == wantRingH &&
        composedW == wantComposedW && composedH == wantComposedH && ring.r != nullptr)
        return true;

    ParkAll();
    const auto make = [&](Tex& t, DXGI_FORMAT format, unsigned int w, unsigned int h)
    {
        t.r = CreateScratch(f.device, format, w, h);
        t.s = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        return t.r != nullptr;
    };
    unsigned int atlasW = 0;
    for (uint32_t level = 0; level < kDlssNrEditCacheCellLevels; ++level)
        atlasW += Cells(in.width, level);
    // 4 slices across, 2 down: inside D3D12's 16384-texel limit up to 4096 x 8192 vectors a slice
    const unsigned int ringTexW = wantRingW * kDlssNrEditCacheRingColumns;
    const unsigned int ringTexH = wantRingH * kDlssNrEditCacheRingRows;
    bool ok = ringTexW <= D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION && ringTexH <= D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION &&
              make(ring, DXGI_FORMAT_R32G32_FLOAT, ringTexW, ringTexH);
    ok = ok && make(anchorEdit, DXGI_FORMAT_R16G16B16A16_FLOAT, in.width, in.height);
    ok = ok && make(anchorPrint, DXGI_FORMAT_R16G16B16A16_FLOAT, in.width, in.height);
    ok = ok && make(context, DXGI_FORMAT_R32_FLOAT, (in.width + 3) / 4, (in.height + 3) / 4);
    ok = ok && make(carried, DXGI_FORMAT_R16G16B16A16_FLOAT, in.width, in.height);
    ok = ok && make(valid[0], DXGI_FORMAT_R16_FLOAT, in.width, in.height);
    ok = ok && make(valid[1], DXGI_FORMAT_R16_FLOAT, in.width, in.height);
    ok = ok && make(atlasEdit, DXGI_FORMAT_R32G32B32A32_FLOAT, atlasW, Cells(in.height, 0));
    ok = ok && make(atlasLuma, DXGI_FORMAT_R32_FLOAT, atlasW, Cells(in.height, 0));
    ok = ok && make(level0Edit, DXGI_FORMAT_R32G32B32A32_FLOAT, atlasW, Cells(in.height, 0));
    ok = ok && make(level0Luma, DXGI_FORMAT_R32_FLOAT, atlasW, Cells(in.height, 0));
    ok = ok && make(shown, DXGI_FORMAT_R16G16B16A16_FLOAT, in.width, in.height);
    ok = ok && make(fadeSource, DXGI_FORMAT_R16G16B16A16_FLOAT, in.width, in.height);
    ok = ok && make(composed, DXGI_FORMAT_R32G32_FLOAT, wantComposedW, wantComposedH);
    if (!ok)
    {
        ParkAll();
        failed = true;
        failedW = in.width;
        failedH = in.height;
        failedRingW = wantRingW;
        failedRingH = wantRingH;
        LOG_ERROR("DLSS-NR edit cache: could not allocate its textures for {}x{} (vectors {}x{}); every frame runs the "
                  "model until a size changes", in.width, in.height, wantRingW, wantRingH);
        return false;
    }
    sizeW = in.width;
    sizeH = in.height;
    ringW = wantRingW;
    ringH = wantRingH;
    composedW = wantComposedW;
    composedH = wantComposedH;
    curValid = 0;
    LOG_INFO("DLSS-NR edit cache: textures for {}x{} (vectors {}x{})", sizeW, sizeH, ringW, ringH);
    return true;
}

void Common(const Frame& f, DlssNrEditCacheMode mode)
{
    const auto& in = f.inputs;
    params = {};
    params.Mode = mode;
    params.Width = in.width;
    params.Height = in.height;
    params.MotionWidth = in.motionWidth;
    params.MotionHeight = in.motionHeight;
    params.MotionBaseX = f.motionBaseX;
    params.MotionBaseY = f.motionBaseY;
    params.DepthWidth = f.depth != nullptr ? f.depthWidth : 0u;
    params.DepthHeight = f.depth != nullptr ? f.depthHeight : 0u;
    params.DepthBaseX = f.depthBaseX;
    params.DepthBaseY = f.depthBaseY;
    params.DepthInverted = f.depthInverted ? 1u : 0u;
    params.MvScaleX = f.mvScaleX;
    params.MvScaleY = f.mvScaleY;
    params.Passthrough = f.passthrough ? 1u : 0u;
    params.Epsilon = std::max(f.passthrough ? 1.0f : f.white, 1e-6f) / 512.0f;
    params.RingSlice = step.ringSlice;
    params.TraceSteps = step.traceSteps;
    params.FadeSteps = step.fadeSteps;
    params.FadeWeight = step.fadeWeight;
    params.Tolerance = kDlssNrEditCacheTolerance;
    params.ContextTolerance = kDlssNrEditCacheContextTolerance;
    params.FirstCarried = step.firstCarried ? 1u : 0u;
    params.FillSigma = kDlssNrEditCacheFillSigma;
    params.DebugView = f.cfg->DlssNrEditCacheDebug.value_or_default();
    params.Age = step.age;
    uint32_t atlasX = 0;
    for (uint32_t level = 0; level < kDlssNrEditCacheCellLevels; ++level)
    {
        SetLevel(params, level, atlasX, Cells(in.width, level), Cells(in.height, level));
        atlasX += Cells(in.width, level);
    }
    params.ComposeWidth = composedW;
    params.ComposeHeight = composedH;
    params.RingWidth = ringW;
    params.RingHeight = ringH;
}

bool Run(const Frame& f, unsigned int w, unsigned int h, std::initializer_list<ID3D12Resource*> inputs, Tex& out0,
         Tex* out1 = nullptr)
{
    ID3D12Resource* srvs[kInputs] = {};
    unsigned int i = 0;
    for (ID3D12Resource* r : inputs)
        if (i < kInputs)
            srvs[i++] = r;
    out0.Write(f.cmdList);
    if (out1 != nullptr)
        out1->Write(f.cmdList);
    return f.pass->DispatchEditCache(f.cmdList, params, w, h, srvs, out0.r, out1 != nullptr ? out1->r : nullptr);
}

void Say(const Frame& f, const DlssNrEditCache::Step& s)
{
    const char* now = !f.inputs.enabled ? "off" : s.standsAside != nullptr ? s.standsAside : "on";
    if (now != loggedAside)
    {
        loggedAside = now;
        if (!f.inputs.enabled)
            LOG_INFO("DLSS-NR edit cache: off");
        else if (s.standsAside != nullptr)
            LOG_INFO("DLSS-NR edit cache: stands aside ({}); every frame runs the model", s.standsAside);
        else
        {
            const uint32_t interval =
                std::clamp(f.inputs.interval, DlssNrEditCache::kMinInterval, DlssNrEditCache::kMaxInterval);
            LOG_INFO("DLSS-NR edit cache: on, the model every {} frames, crossfade {}{}", interval,
                     DlssNrEditCache::CrossfadeFor(interval, f.inputs.crossfade),
                     f.inputs.crossfade == 0 ? " (follows the interval)" : "");
        }
    }
    const auto now2 = std::chrono::steady_clock::now();
    if (now2 - shareSince > std::chrono::seconds(1))
    {
        modelShare = cadence.TakeModelShare();
        shareSince = now2;
    }
    publishedRunning = s.runs;
    publishedShare = modelShare;
    publishedAside = s.standsAside;
}

Plan Decide(const Frame& f);

Plan BeforeModel(const Frame& f)
{
    Plan plan = Decide(f);
    // A frame after a skipped one that does not hand the model vectors back to its last run (the cache stood aside,
    // was turned off, or failed) starts the model's history over: it is that many frames old, and this frame's
    // vectors cover one.
    if (lastCached && !plan.cached && plan.motion == f.motion)
        plan.resetModel = true;
    lastCached = plan.cached;
    return plan;
}

Plan Decide(const Frame& f)
{
    Plan plan;
    plan.motion = f.motion;
    plan.motionBaseX = f.motionBaseX;
    plan.motionBaseY = f.motionBaseY;

    DlssNrEditCache::Inputs in = f.inputs;
    // A failed allocation is tried again once a size changes
    if (failed && (in.width != failedW || in.height != failedH || RingWidthFor(f) != failedRingW ||
                   RingHeightFor(f) != failedRingH))
        failed = false;
    in.enabled = in.enabled && !failed && f.pass->EditCacheReady();
    // The textures before the cadence plans the frame: new ones hold nothing, and Allocate's ParkAll starts the cadence
    // over, so the frame planned is a refresh
    if (in.enabled && DlssNrEditCache::StandAsideReason(in) == nullptr && !Allocate(f))
        in.enabled = false;
    step = cadence.Next(in);
    Say(f, step);
    if (!step.runs)
    {
        if (!in.enabled && ring.r != nullptr)
            ParkAll();
        return plan;
    }

    // Every frame: its vectors into the ring
    Common(f, DlssNrEditCache_Ring);
    if (!Run(f, ringW, ringH, { f.motion, f.depth != nullptr ? f.depth : f.motion }, ring))
    {
        cadence.Drop();
        step = {};
        return plan;
    }
    ring.Read(f.cmdList);

    plan.runs = true;
    plan.refresh = step.refresh;
    if (step.refresh)
    {
        // The model's own history: the vectors back to its last run, or a fresh start where none reach back
        if (step.composeForModel)
        {
            Common(f, DlssNrEditCache_Compose);
            if (Run(f, composedW, composedH, { ring.r }, composed))
            {
                // Sized like the motion texture's allocation, the vectors in the subrect's own place: the origin stays
                composed.Read(f.cmdList);
                plan.motion = composed.r;
            }
            else
                plan.resetModel = true;
        }
        else
            plan.resetModel = true;
        return plan;
    }

    // A cached frame: the anchor's edit carried here, checked, and the surface fill's grid; ApplyCached writes the
    // output where the resolve would have
    Common(f, DlssNrEditCache_Context);
    Run(f, (in.width + 3) / 4, (in.height + 3) / 4, { f.frame }, context);
    context.Read(f.cmdList);

    Tex& validNow = valid[curValid];
    Tex& validPrev = valid[curValid ^ 1u];
    validPrev.Read(f.cmdList);
    anchorEdit.Read(f.cmdList);
    anchorPrint.Read(f.cmdList);
    Common(f, DlssNrEditCache_Carry);
    bool ok = Run(f, in.width, in.height, { f.frame, ring.r, anchorEdit.r, anchorPrint.r, context.r, validPrev.r },
                  carried, &validNow);
    curValid ^= 1u;
    carried.Read(f.cmdList);

    // Level 0 from the pixels, then a copy of it, then the coarser levels from the copy's cells (each level writes its
    // own region of the atlases; they are read only by Apply)
    Common(f, DlssNrEditCache_Cells);
    for (uint32_t level = 0; ok && level < kDlssNrEditCacheCellLevels; ++level)
    {
        params.CellLevel = level;
        atlasEdit.Write(f.cmdList);
        atlasLuma.Write(f.cmdList);
        if (level == 1)
        {
            atlasEdit.To(f.cmdList, D3D12_RESOURCE_STATE_COPY_SOURCE);
            atlasLuma.To(f.cmdList, D3D12_RESOURCE_STATE_COPY_SOURCE);
            level0Edit.To(f.cmdList, D3D12_RESOURCE_STATE_COPY_DEST);
            level0Luma.To(f.cmdList, D3D12_RESOURCE_STATE_COPY_DEST);
            f.cmdList->CopyResource(level0Edit.r, atlasEdit.r);
            f.cmdList->CopyResource(level0Luma.r, atlasLuma.r);
            level0Edit.Read(f.cmdList);
            level0Luma.Read(f.cmdList);
            atlasEdit.Write(f.cmdList);
            atlasLuma.Write(f.cmdList);
        }
        // Level 0 reads no copy (it is not made yet, and not in a readable state): the frame stands in
        ok = f.pass->DispatchEditCache(f.cmdList, params, LevelCountX(params, level), LevelCountY(params, level),
                                       { f.frame, carried.r, level == 0 ? f.frame : level0Edit.r,
                                         level == 0 ? f.frame : level0Luma.r, f.frame, f.frame },
                                       atlasEdit.r, atlasLuma.r);
    }
    if (!ok)
    {
        // The output still has to be written this frame: run the model after all
        cadence.Drop();
        step = {};
        plan.runs = false;
        plan.resetModel = true;
        return plan;
    }
    atlasEdit.Read(f.cmdList);
    atlasLuma.Read(f.cmdList);
    plan.cached = true;
    return plan;
}

// The model's composed vectors are read: back to rest. Every frame the cache ran, after the model.
void AfterModel(const Frame& f)
{
    composed.Write(f.cmdList);
}

// A cached frame: frame x 2^(carried edit, filled, crossfaded) into the output (UNORDERED_ACCESS).
bool ApplyCached(const Frame& f, ID3D12Resource* output)
{
    Common(f, DlssNrEditCache_Apply);
    fadeSource.Read(f.cmdList);
    ID3D12Resource* srvs[kInputs] = { f.frame, carried.r, atlasEdit.r, atlasLuma.r, fadeSource.r,
                                                     ring.r };
    shown.Write(f.cmdList);
    return f.pass->DispatchEditCache(f.cmdList, params, f.inputs.width, f.inputs.height, srvs, output, shown.r);
}

// A refresh, after the resolve: the output (readable here) becomes the anchor. With a crossfade the output is then
// rewritten from what was shown before (the caller makes it writable again in between: `makeWritable`).
template <typename MakeWritable>
void AfterResolve(const Frame& f, ID3D12Resource* output, MakeWritable makeWritable)
{
    if (!step.runs || !step.refresh)
        return;
    const auto& in = f.inputs;

    // What was shown up to now is what the crossfade starts from
    if (step.fadeSteps > 0)
    {
        shown.To(f.cmdList, D3D12_RESOURCE_STATE_COPY_SOURCE);
        fadeSource.To(f.cmdList, D3D12_RESOURCE_STATE_COPY_DEST);
        f.cmdList->CopyResource(fadeSource.r, shown.r);
    }

    Common(f, DlssNrEditCache_Context);
    Run(f, (in.width + 3) / 4, (in.height + 3) / 4, { f.frame }, context);
    context.Read(f.cmdList);
    Common(f, DlssNrEditCache_Capture);
    Run(f, in.width, in.height, { f.frame, output, f.frame, f.frame, context.r }, anchorEdit, &anchorPrint);
    anchorEdit.Read(f.cmdList);

    makeWritable();
    if (step.fadeSteps > 0)
    {
        // The crossfade's first frame: the new answer at 1/crossfade, the rest what was shown, carried one frame
        Common(f, DlssNrEditCache_Apply);
        fadeSource.Read(f.cmdList);
        ID3D12Resource* srvs[kInputs] = { f.frame, anchorEdit.r, f.frame, f.frame, fadeSource.r, ring.r };
        // Every pixel of the anchor is believed (its alpha is 1), so the cell atlases are not read
        shown.Write(f.cmdList);
        f.pass->DispatchEditCache(f.cmdList, params, in.width, in.height, srvs, output, shown.r);
    }
    else
    {
        // Shown on this frame: the anchor itself
        anchorEdit.To(f.cmdList, D3D12_RESOURCE_STATE_COPY_SOURCE);
        shown.To(f.cmdList, D3D12_RESOURCE_STATE_COPY_DEST);
        f.cmdList->CopyResource(shown.r, anchorEdit.r);
    }
}

// Everything back to rest at the end of the frame (the next frame starts from UNORDERED_ACCESS)
void EndFrame(ID3D12GraphicsCommandList* cmdList)
{
    for (Tex* t : AllTextures)
        t->Write(cmdList);
}

void Release()
{
    for (Tex* t : AllTextures)
    {
        if (t->r != nullptr)
            t->r->Release();
        t->r = nullptr;
        t->s = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    }
    sizeW = sizeH = ringW = ringH = composedW = composedH = 0;
    cadence.Drop();
}

// For the menu: on, the model's share of frames, why it stands aside
bool Running() { return publishedRunning; }
float ModelShare() { return publishedShare; }
const char* StandsAside() { return publishedAside; }
} // namespace EditCache
