// Writes out the frames this pass is arguing about, so questions get settled by measurement.
//
// Every comparison so far has been two separate video captures: different camera paths, different
// exposure, and H.264 in between throwing away exactly the high-frequency temporal detail the argument
// is about. Normalising for all that leaves conclusions that need hedging.
//
// This captures the same frames twice instead. The pass already holds the frame as the upscaler produced
// it and the frame after the model's edit, so both are written for the same run of consecutive frames --
// a perfect control, with nothing varying but the thing under test. Flicker in one against flicker in
// the other is then a direct answer rather than an inference.
//
// A run can also carry more images per frame (the edit cache dump adds the game's motion vectors and depth, so an
// offline replay can carry one frame's edit to the next), and a line of notes per frame for what the images alone do
// not say (the vectors' scale and subrect, which way depth runs, the white point).
//
// Raw, because a codec is the confound. A manifest alongside says how to read them.

#pragma once

#include <windows.h>
#include <d3d12.h>

#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

namespace capture
{
// Bounded on purpose: this writes hundreds of megabytes and must not be able to fill a drive. Each run
// overwrites the last.
constexpr unsigned int kMaxFrames = 8;
// The edit cache dump: long enough to measure carrying an edit 8 frames on a few starting frames.
constexpr unsigned int kMaxDumpFrames = 32;
// And never more than this in readback buffers (system memory), whatever the frame count asks: a run is shortened
// to fit. 16 frames of a 1440p half-float before/after pair with render-size vectors and depth is about 1.2 GB.
constexpr unsigned long long kMaxBytes = 1536ull << 20;

// One image of a frame: its name in the files ("before" -> before_NN.raw) and the state it is in when recorded.
struct Source
{
    const char* name = nullptr;
    ID3D12Resource* resource = nullptr;
    D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
};

struct Shot
{
    ID3D12Resource* readback = nullptr;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout = {};
    unsigned long long bytes = 0;
};

// Captures a run of consecutive frames of several images at once.
class FrameCapture
{
  public:
    // Asks for a capture. Ignored if one is already running.
    void request(unsigned int frames, unsigned int maxFrames = kMaxFrames)
    {
        if (active_)
            return;

        wanted_ = frames > maxFrames ? maxFrames : frames;
        captured_ = 0;
        active_ = wanted_ > 0;
    }

    bool isActive() const { return active_; }
    unsigned int progress() const { return captured_; }
    unsigned int wanted() const { return wanted_; }

    // Records copies of every image for this frame, each in the state given. The first frame fixes which images a
    // run has; a frame that brings a different set ends the run there (a guide that went away, say).
    void record(ID3D12GraphicsCommandList* cmd, ID3D12Device* device, const std::vector<Source>& sources,
                const std::string& note = {})
    {
        if (!active_ || sources.empty())
            return;

        for (const Source& s : sources)
            if (s.resource == nullptr || s.name == nullptr)
                return;

        // Recording stops the moment the run is complete, and does not resume until write() has
        // released everything.
        //
        // ready_ is set here but acted on eight frames later, because the caller has to let the GPU
        // finish with these copies before mapping them. Without this guard those eight frames each
        // recorded another one: captured_ walked past the end of a vector sized to exactly wanted_,
        // and the garbage read back as a Shot had its pointer handed to CopyTextureRegion. The device
        // was removed and the game went down with nothing in the log -- every single time anyone
        // pressed the button.
        if (ready_ || captured_ >= wanted_)
            return;

        if (!ensure(device, sources))
        {
            release();
            return;
        }

        if (!sameSet(sources))
        {
            ready_ = captured_ > 0;
            active_ = ready_;
            return;
        }

        // ensure() sizes the vectors to wanted_. Belt and braces: an index into them is never taken
        // on trust again.
        for (const Channel& c : channels_)
        {
            if (captured_ >= c.shots.size())
            {
                ready_ = true;
                return;
            }
        }

        for (size_t i = 0; i < sources.size(); ++i)
            copy(cmd, sources[i].resource, sources[i].state, channels_[i].shots[captured_]);

        notes_.push_back(note);
        ++captured_;

        if (captured_ >= wanted_)
            ready_ = true;
    }

    // True once every frame has been copied and the GPU has been waited for. The caller does the waiting,
    // since it owns the fence.
    bool readyToWrite() const { return ready_; }

    // Writes what was captured and releases everything. Returns the directory, or an empty string.
    std::string write(const std::filesystem::path& directory, const std::string& about = {})
    {
        if (!ready_)
            return {};

        // A dark frame -- a menu, a loading screen -- measures nothing. Discard the run and quietly
        // re-arm for the same length, so the set that finally lands is of actual gameplay.
        if (captured_ > 0 && !channels_.empty() && isDark(channels_[0].shots[0]))
        {
            const unsigned int frames = wanted_;
            release();
            request(frames, frames);
            return {};
        }

        std::error_code ec;
        std::filesystem::create_directories(directory, ec);

        for (unsigned int i = 0; i < captured_; ++i)
            for (Channel& c : channels_)
                dump(directory, c.name.c_str(), i, c.shots[i]);

        writeManifest(directory, about);
        release();
        return directory.string();
    }

    void release()
    {
        for (Channel& c : channels_)
            for (Shot& s : c.shots)
                if (s.readback != nullptr)
                    s.readback->Release();

        channels_.clear();
        notes_.clear();
        active_ = false;
        ready_ = false;
        captured_ = 0;
    }

  private:
    struct Channel
    {
        std::string name;
        D3D12_RESOURCE_DESC desc = {};
        std::vector<Shot> shots;
    };

    bool ensure(ID3D12Device* device, const std::vector<Source>& sources)
    {
        if (!channels_.empty())
            return true;

        channels_.resize(sources.size());
        unsigned long long perFrame = 0;

        for (size_t i = 0; i < sources.size(); ++i)
        {
            channels_[i].name = sources[i].name;
            channels_[i].desc = sources[i].resource->GetDesc();
            channels_[i].desc.Format = TypedForCopy(channels_[i].desc.Format);
            perFrame += FootprintBytes(device, channels_[i].desc);
        }

        // Shortened to fit the budget, but never below two frames (nothing to compare otherwise).
        if (perFrame == 0)
            return false;
        while (wanted_ > 2 && perFrame * wanted_ > kMaxBytes)
            --wanted_;
        if (perFrame * wanted_ > kMaxBytes)
            return false;

        for (Channel& c : channels_)
        {
            c.shots.resize(wanted_);
            for (unsigned int f = 0; f < wanted_; ++f)
                if (!alloc(device, c.desc, c.shots[f]))
                    return false;
        }

        return true;
    }

    bool sameSet(const std::vector<Source>& sources) const
    {
        if (sources.size() != channels_.size())
            return false;

        for (size_t i = 0; i < sources.size(); ++i)
        {
            const D3D12_RESOURCE_DESC d = sources[i].resource->GetDesc();
            if (channels_[i].name != sources[i].name || d.Width != channels_[i].desc.Width ||
                d.Height != channels_[i].desc.Height || TypedForCopy(d.Format) != channels_[i].desc.Format)
                return false;
        }

        return true;
    }

    // A copy needs a fully typed format, and the buffer the upscaler writes is occasionally declared
    // typeless. A placed footprint carrying a typeless format makes CopyTextureRegion invalid, and an
    // invalid copy removes the device -- which is what capturing did: it crashed the game every time
    // the output happened to be one of these. Guides add their own typeless formats (the pass clones
    // typeless depth and vectors to typed ones, so these are a second line).
    static DXGI_FORMAT TypedForCopy(DXGI_FORMAT f)
    {
        switch (f)
        {
        case DXGI_FORMAT_R16G16B16A16_TYPELESS:
            return DXGI_FORMAT_R16G16B16A16_FLOAT;
        case DXGI_FORMAT_R32G32B32A32_TYPELESS:
            return DXGI_FORMAT_R32G32B32A32_FLOAT;
        case DXGI_FORMAT_R10G10B10A2_TYPELESS:
            return DXGI_FORMAT_R10G10B10A2_UNORM;
        case DXGI_FORMAT_R8G8B8A8_TYPELESS:
            return DXGI_FORMAT_R8G8B8A8_UNORM;
        case DXGI_FORMAT_B8G8R8A8_TYPELESS:
            return DXGI_FORMAT_B8G8R8A8_UNORM;
        case DXGI_FORMAT_R16G16_TYPELESS:
            return DXGI_FORMAT_R16G16_FLOAT;
        case DXGI_FORMAT_R32G32_TYPELESS:
            return DXGI_FORMAT_R32G32_FLOAT;
        case DXGI_FORMAT_R32_TYPELESS:
            return DXGI_FORMAT_R32_FLOAT;
        case DXGI_FORMAT_R16_TYPELESS:
            return DXGI_FORMAT_R16_UNORM;
        default:
            return f;
        }
    }

    static unsigned long long FootprintBytes(ID3D12Device* device, const D3D12_RESOURCE_DESC& desc)
    {
        unsigned long long total = 0;
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout = {};
        device->GetCopyableFootprints(&desc, 0, 1, 0, &layout, nullptr, nullptr, &total);
        return total == ~0ull ? 0 : total;
    }

    static bool alloc(ID3D12Device* device, const D3D12_RESOURCE_DESC& desc, Shot& shot)
    {
        unsigned long long total = 0;
        device->GetCopyableFootprints(&desc, 0, 1, 0, &shot.layout, nullptr, nullptr, &total);
        shot.bytes = total;

        if (total == 0 || total == ~0ull)
            return false;

        D3D12_HEAP_PROPERTIES heap = {};
        heap.Type = D3D12_HEAP_TYPE_READBACK;

        D3D12_RESOURCE_DESC buf = {};
        buf.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        buf.Width = total;
        buf.Height = 1;
        buf.DepthOrArraySize = 1;
        buf.MipLevels = 1;
        buf.Format = DXGI_FORMAT_UNKNOWN;
        buf.SampleDesc.Count = 1;
        buf.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        return SUCCEEDED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buf,
                                                         D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                         IID_PPV_ARGS(&shot.readback)));
    }

    static void copy(ID3D12GraphicsCommandList* cmd, ID3D12Resource* src, D3D12_RESOURCE_STATES state,
                     Shot& shot)
    {
        if (shot.readback == nullptr)
            return;

        D3D12_RESOURCE_BARRIER b = {};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = src;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = state;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;

        const bool needsTransition = state != D3D12_RESOURCE_STATE_COPY_SOURCE;

        if (needsTransition)
            cmd->ResourceBarrier(1, &b);

        D3D12_TEXTURE_COPY_LOCATION dst = {};
        dst.pResource = shot.readback;
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint = shot.layout;

        D3D12_TEXTURE_COPY_LOCATION source = {};
        source.pResource = src;
        source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        source.SubresourceIndex = 0;

        cmd->CopyTextureRegion(&dst, 0, 0, 0, &source, nullptr);

        if (needsTransition)
        {
            b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
            b.Transition.StateAfter = state;
            cmd->ResourceBarrier(1, &b);
        }
    }

    // Mean byte value across the image, sampled sparsely. Correct for 8-bit and 10-bit surfaces;
    // float surfaces of dark content read higher, which only means a float capture is never discarded.
    static bool isDark(Shot& shot)
    {
        if (shot.readback == nullptr || shot.bytes == 0)
            return false;

        void* mapped = nullptr;
        D3D12_RANGE range = { 0, (SIZE_T) shot.bytes };

        if (FAILED(shot.readback->Map(0, &range, &mapped)) || mapped == nullptr)
            return false;

        const unsigned char* p = (const unsigned char*) mapped;
        unsigned long long total = 0;
        unsigned long long count = 0;

        for (unsigned long long i = 0; i < shot.bytes; i += 1021)
        {
            total += p[i];
            ++count;
        }

        D3D12_RANGE written = { 0, 0 };
        shot.readback->Unmap(0, &written);
        return count > 0 && total / count < 4;
    }

    static void dump(const std::filesystem::path& dir, const char* which, unsigned int index, Shot& shot)
    {
        if (shot.readback == nullptr)
            return;

        void* mapped = nullptr;
        D3D12_RANGE range = { 0, (SIZE_T) shot.bytes };

        if (FAILED(shot.readback->Map(0, &range, &mapped)) || mapped == nullptr)
            return;

        char name[64];
        std::snprintf(name, sizeof(name), "%s_%02u.raw", which, index);
        const auto path = dir / name;

        if (std::FILE* f = _wfopen(path.wstring().c_str(), L"wb"))
        {
            std::fwrite(mapped, 1, (size_t) shot.bytes, f);
            std::fclose(f);
        }

        D3D12_RANGE written = { 0, 0 };
        shot.readback->Unmap(0, &written);
    }

    void writeManifest(const std::filesystem::path& dir, const std::string& about)
    {
        const auto path = dir / "manifest.txt";

        if (std::FILE* f = _wfopen(path.wstring().c_str(), L"wt"))
        {
            std::fprintf(f, "frames %u\n", captured_);
            for (const Channel& c : channels_)
                std::fprintf(f, "%s width %llu height %u format %d rowPitch %u copyFormat %d\n", c.name.c_str(),
                             (unsigned long long) c.desc.Width, c.desc.Height, (int) c.desc.Format,
                             c.shots.empty() ? 0 : c.shots[0].layout.Footprint.RowPitch,
                             c.shots.empty() ? 0 : (int) c.shots[0].layout.Footprint.Format);
            for (unsigned int i = 0; i < captured_ && i < notes_.size(); ++i)
                if (!notes_[i].empty())
                    std::fprintf(f, "frame %02u %s\n", i, notes_[i].c_str());
            std::fprintf(f, "\nbefore_NN.raw is the frame as the upscaler produced it (a frame noted beforeIsProxy 1: the "
                            "model's input picture instead).\n");
            std::fprintf(f, "after_NN.raw is the same frame once the model's edit was applied.\n");
            std::fprintf(f, "Consecutive frames, same run, so the pair is a control.\n");
            if (!about.empty())
                std::fprintf(f, "%s\n", about.c_str());
            std::fclose(f);
        }
    }

    std::vector<Channel> channels_;
    std::vector<std::string> notes_;
    unsigned int wanted_ = 0;
    unsigned int captured_ = 0;
    bool active_ = false;
    bool ready_ = false;
};
} // namespace capture
