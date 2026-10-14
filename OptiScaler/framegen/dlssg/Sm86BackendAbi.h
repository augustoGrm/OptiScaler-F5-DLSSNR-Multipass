#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>

// Private, versioned x64 ABI of the backend in SilyNoMeta/dlssg_for_sm86
// 0.3.5-5 resource 31000 -> resource 30002. SHA256:
// 6cc675209e55bb3ddb6b1ba8008eef93f633d3e2207c3a073df321a6a8ed4e59.
// The binary ships no SDK. These layouts and defaults were checked against its
// exports AND the corresponding calls in the pinned resource-31000 proxy.
// Never bind this ABI to an unverified replacement DLL.
namespace AmpereMfgLoader::BackendAbi
{
inline constexpr uint32_t kMaxGeneratedFrames = 5;
inline constexpr char kRuntimeSha256[] = "ff6e90eb78b827927dff5b4ecc6b1c870c2e9bca29ed9f48c7d348cc9e170b82";

// Exact flags produced by the pinned proxy for Optimized=1, Auto kernel image,
// default compatibility settings and MaxGeneratedFrames=5. Its flag builder at
// RVA 0x1a590 yields 0x180040; the install caller at 0x17a3c adds 0x0d.
// This preserves its default image patches and does not enable ImageApprox,
// hardware bilinear filtering, debug markers or a forced per-frame multiplier.
inline constexpr uint32_t kOptimizedDefaultFlags = 0x0018004d;

struct Info
{
    uint32_t size = 32;
    uint32_t version = 1;
    uint32_t maxGeneratedFrames = 0;
    uint32_t reserved = 0;
    const char* name = nullptr;
    const char* runtimeSha256 = nullptr;
};

struct RuntimeConfig
{
    uint32_t size = 44;
    uint32_t version = 1;
    uint32_t maxGeneratedFrames = kMaxGeneratedFrames;
    uint32_t logLevel = 0;
    uint32_t logFile = 0;
    uint32_t debugOutput = 0;
    uint32_t evaluateEvery = 120;
    uint32_t markGeneratedFrames = 0;
    uint32_t markerX = 8;
    uint32_t markerY = 8;
    uint32_t markerScale = 2;
};

struct Install
{
    uint32_t size = 80;
    uint32_t version = 1;
    void* runtime = nullptr;
    uint32_t flags = kOptimizedDefaultFlags;
    uint32_t reserved = 0;
    const wchar_t* logPath = nullptr;
    RuntimeConfig config {};
    uint32_t tailPadding = 0;
};

// Plugin capability ceiling only. Actual frame count continues to come from
// OptiScaler's slDLSSGSetOptions for the active viewport, including dynamic mode.
struct PluginConfig
{
    uint32_t size = 12;
    uint32_t version = 1;
    uint32_t maxGeneratedFrames = kMaxGeneratedFrames;
};

struct ImageApproxConfig
{
    uint32_t size = 20;
    uint32_t version = 1;
    uint32_t mask = 0;
    uint32_t variant = 0;
    uint32_t overrideVariant = 0;
};

static_assert(sizeof(void*) == 8, "The pinned SM86 backend only supports x64");
static_assert(std::is_standard_layout_v<Install> && std::is_standard_layout_v<Info>);
static_assert(sizeof(Info) == 32 && offsetof(Info, name) == 16 && offsetof(Info, runtimeSha256) == 24);
static_assert(sizeof(RuntimeConfig) == 44 && offsetof(RuntimeConfig, maxGeneratedFrames) == 8);
static_assert(sizeof(Install) == 80 && offsetof(Install, runtime) == 8 && offsetof(Install, flags) == 16 &&
              offsetof(Install, logPath) == 24 && offsetof(Install, config) == 32);
static_assert(sizeof(PluginConfig) == 12 && offsetof(PluginConfig, maxGeneratedFrames) == 8);
static_assert(sizeof(ImageApproxConfig) == 20 && offsetof(ImageApproxConfig, overrideVariant) == 16);
} // namespace AmpereMfgLoader::BackendAbi
