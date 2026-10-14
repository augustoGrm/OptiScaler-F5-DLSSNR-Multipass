#include "pch.h"

#include "AmpereMfgLoader.h"

#include <Config.h>
#include <State.h>
#include <Util.h>
#include <misc/IdentifyGpu.h>
#include <proxies/Ntdll_Proxy.h>

#include <fstream>
#include <sstream>
#include <mutex>
#include <bcrypt.h>
#include <SimpleIni.h>
#include <proxies/KernelBase_Proxy.h>
#pragma comment(lib, "bcrypt.lib")

#ifndef NV_GPU_ARCHITECTURE_GA100
#define NV_GPU_ARCHITECTURE_GA100 0x00000170
#endif

namespace AmpereMfgLoader
{
namespace
{
Status s_status;
bool s_setupAttempted = false;
std::recursive_mutex s_mutex;
HMODULE s_runtime = nullptr;
using RequestFn = BOOL(WINAPI*)(const Control::Request*);
using StatusFn = BOOL(WINAPI*)(Control::RuntimeStatus*);
RequestFn s_request = nullptr;
StatusFn s_getStatus = nullptr;
std::optional<Control::Request> s_lastRequest;
std::optional<Control::Request> s_lastRefusedRequest;

constexpr unsigned char kRuntimeSha[32] = { 0x74, 0x89, 0xa8, 0x9b, 0xf5, 0x93, 0xcd, 0xa2, 0x43, 0xd9, 0x5c,
                                            0x62, 0xe6, 0xd8, 0xb7, 0x59, 0x05, 0xd9, 0x77, 0x4e, 0x27, 0xb7,
                                            0xb2, 0xd5, 0x3b, 0x91, 0x62, 0x5a, 0x79, 0xc0, 0x5c, 0xa4 };

bool Verified(const void* data, DWORD bytes)
{
    unsigned char hash[32] {};
    return data != nullptr && bytes == 33231360 &&
           BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0, (PUCHAR) data, bytes, hash, sizeof(hash)) == 0 &&
           memcmp(hash, kRuntimeSha, sizeof(hash)) == 0;
}

bool VerifiedFile(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file || file.tellg() != 33231360)
        return false;
    std::vector<char> bytes(33231360);
    file.seekg(0);
    file.read(bytes.data(), bytes.size());
    return file && Verified(bytes.data(), (DWORD) bytes.size());
}

bool ExtractEmbedded(const std::filesystem::path& path)
{
    auto resource = FindResourceW(dllModule, MAKEINTRESOURCEW(31010), MAKEINTRESOURCEW(10));
    if (!resource)
        return false;
    auto bytes = SizeofResource(dllModule, resource);
    auto loaded = LoadResource(dllModule, resource);
    auto data = loaded ? LockResource(loaded) : nullptr;
    if (!Verified(data, bytes))
        throw std::runtime_error("Embedded SM86 payload does not match the pinned 0.3.5-5 image");
    s_status.Embedded = true;
    if (VerifiedFile(path))
        return true;
    std::filesystem::create_directories(path.parent_path());
    const auto temp = path.wstring() + L"." + std::to_wstring(GetCurrentProcessId()) + L".tmp";
    std::ofstream file(temp, std::ios::binary | std::ios::trunc);
    file.write((const char*) data, bytes);
    file.close();
    if (!file)
        throw std::runtime_error("Could not extract embedded SM86 runtime");
    if (!MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    {
        DeleteFileW(temp.c_str());
        if (!VerifiedFile(path))
            throw std::runtime_error("Could not install the extracted SM86 runtime");
    }
    return true;
}

void PrepareIni(const std::filesystem::path& path)
{
    // Preserve other settings in the game's existing file; only our runtime keys
    // are updated. This is the executable directory used by the pinned proxy.
    CSimpleIniA ini;
    ini.SetUnicode();
    if (std::filesystem::exists(path) && ini.LoadFile(path.c_str()) < 0)
        throw std::runtime_error("Cannot read the SM86 configuration");
    ini.SetValue("General", "Enabled", "1");
    ini.SetValue("FrameGeneration", "Optimized", "1");
    const auto max = std::clamp(Config::Instance()->FGDLSSGAmpereMfgMaxFrames.value_or_default(), 1, 5);
    ini.SetValue("FrameGeneration", "MaxGeneratedFrames", std::to_string(max).c_str());
    ini.SetValue("Runtime", "Mode", "Bundled");
    if (ini.SaveFile(path.c_str()) < 0)
        throw std::runtime_error("Cannot write the SM86 configuration");
}

} // namespace

Status LastStatus()
{
    std::lock_guard lock(s_mutex);
    return s_status;
}

std::string ResolveAutoKernelImage()
{
    const auto& gpu = IdentifyGpu::getPrimaryGpu();
    const bool onLinux = State::Instance().isRunningOnLinux || gpu.d3d12IsVkd3dProton;
    return ResolveAutoKernelImage(static_cast<uint32_t>(gpu.nvidiaArchInfo.architecture_id), gpu.name, onLinux);
}

std::string ResolveRouter()
{
    const auto& gpu = IdentifyGpu::getPrimaryGpu();
    return ResolveRouter(static_cast<uint32_t>(gpu.nvidiaArchInfo.architecture_id), gpu.name);
}

std::string GenerateIniContent()
{
    auto* cfg = Config::Instance();
    int maxFrames = cfg->FGDLSSGAmpereMfgMaxFrames.value_or_default();

    std::string kernelImg = cfg->FGDLSSGAmpereMfgKernelImage.value_or("Auto");
    if (kernelImg != "PTX" && kernelImg != "Cubin")
        kernelImg = "Auto";

    if (kernelImg == "Auto")
    {
        std::string resolved = ResolveAutoKernelImage();
        if (resolved != "Auto")
        {
            LOG_INFO("AmpereMfgLoader: Auto kernel image resolved to {} for GPU: {}", resolved,
                     IdentifyGpu::getPrimaryGpu().name);
            kernelImg = resolved;
        }
    }

    int hwBilinear = cfg->FGDLSSGAmpereMfgHardwareBilinear.value_or_default() ? 1 : 0;
    std::string router = ResolveRouter();
    int logLevel = 1;

    LOG_INFO("AmpereMfgLoader: Router selected: {} for GPU: {}", router, IdentifyGpu::getPrimaryGpu().name);

    return FormatIniContent(maxFrames, kernelImg, hwBilinear, router, logLevel);
}

void TrySetup()
{
    std::lock_guard lock(s_mutex);
    if (s_setupAttempted)
        return;
    s_setupAttempted = true;

    auto* cfg = Config::Instance();
    if (!cfg->FGDLSSGAmpereMfgUnlock.value_or_default())
        return;

    s_status.Enabled = true;

    // Mutual exclusion: fail if Ada MFG unlock is also enabled
    if (cfg->FGDLSSGAdaMfgUnlock.value_or_default())
    {
        s_status.ErrorMessage = "Cannot enable SM86/SM75 MFG while Ada (RTX 40) MFG unlock is enabled.";
        LOG_ERROR("AmpereMfgLoader: {}", s_status.ErrorMessage);
        return;
    }

    // GPU guard: verify Nvidia Turing or Ampere architecture
    const auto& gpu = IdentifyGpu::getPrimaryGpu();
    if (gpu.vendorId != VendorId::Nvidia)
    {
        s_status.ErrorMessage = "SM86/SM75 MFG requires an NVIDIA GPU.";
        LOG_ERROR("AmpereMfgLoader: {}", s_status.ErrorMessage);
        return;
    }

    const uint32_t archId = static_cast<uint32_t>(gpu.nvidiaArchInfo.architecture_id);
    const bool isAmpere = IsAmpereArch(archId) || (gpu.name.find("RTX 30") != std::string::npos);
    const bool isTuring = IsTuringArch(archId) || (gpu.name.find("RTX 20") != std::string::npos ||
                                                   gpu.name.find("GTX 16") != std::string::npos);

    if (!isAmpere && !isTuring)
    {
        s_status.ErrorMessage = std::format("SM86/SM75 MFG requires an RTX 20 series (Turing) or RTX 30 series "
                                            "(Ampere) GPU. Detected arch 0x{:x} ({}).",
                                            archId, gpu.name);
        LOG_ERROR("AmpereMfgLoader: {}", s_status.ErrorMessage);
        return;
    }

    // A separately installed universal proxy must not hook the same runtime a
    // second time. OptiScaler's own proxy name can match, so inspect its exports.
    for (const wchar_t* name : { L"version.dll", L"dlssg_sm86.dll", L"dinput8.dll", L"dxgi.dll", L"winmm.dll" })
    {
        auto module = GetModuleHandleW(name);
        if (module != nullptr && module != dllModule &&
            KernelBaseProxy::GetProcAddress_()(module, "DLSSG_UniversalProxy") != nullptr)
        {
            s_status.ErrorMessage =
                "A separate SM86 proxy is already loaded. Remove that proxy and restart to use built-in MFG.";
            LOG_ERROR("AmpereMfgLoader: {}", s_status.ErrorMessage);
            return;
        }
    }

    // The build embeds the exact proxy; extraction is automatic and never replaces
    // version.dll beside the game (which might belong to another mod).
    auto basePath = Util::DllPath().parent_path();
    auto dllPath = basePath / L"OptiScaler" / L"dlssg_sm86" / L"version.dll";
    try
    {
        ExtractEmbedded(dllPath);
        if (!VerifiedFile(dllPath))
            throw std::runtime_error("SM86 0.3.5-5 runtime is missing or has an unexpected SHA256");
        s_status.DllFound = true;
        PrepareIni(Util::ExePath().parent_path() / L"dlssg_sm86.ini");
        s_status.IniWritten = true;
    }
    catch (const std::exception& error)
    {
        s_status.ErrorMessage = error.what();
        LOG_ERROR("AmpereMfgLoader: {}", s_status.ErrorMessage);
        return;
    }

    // Load dlssg_sm86.dll
    NtdllProxy::Init();
    LOG_INFO("AmpereMfgLoader: Loading {}", wstring_to_string(dllPath.wstring()));
    HMODULE hMod = NtdllProxy::LoadLibraryExW_Ldr(dllPath.c_str(), NULL, 0);
    if (!hMod)
        hMod = LoadLibraryW(dllPath.c_str());

    if (!hMod)
    {
        DWORD err = GetLastError();
        s_status.DllLoaded = false;
        s_status.ErrorMessage = "Failed to load dlssg_sm86.dll (error code " + std::to_string(err) + ").";
        LOG_ERROR("AmpereMfgLoader: Failed to load dlssg_sm86.dll, error: {}", err);
        return;
    }

    s_runtime = hMod;
    s_request = reinterpret_cast<RequestFn>(KernelBaseProxy::GetProcAddress_()(hMod, "DLSSG_RequestControl"));
    s_getStatus = reinterpret_cast<StatusFn>(KernelBaseProxy::GetProcAddress_()(hMod, "DLSSG_GetControlStatus"));
    if (!s_request || !s_getStatus)
    {
        s_status.ErrorMessage = "Pinned SM86 runtime does not expose its control API";
        LOG_ERROR("AmpereMfgLoader: {}", s_status.ErrorMessage);
        return;
    }
    s_status.DllLoaded = true;
    s_status.ErrorMessage.clear();
    LOG_INFO("AmpereMfgLoader: SM86 MFG loaded successfully from {}", wstring_to_string(dllPath.wstring()));
}

void UpdateControls()
{
    std::lock_guard lock(s_mutex);
    if (!s_status.DllLoaded || !s_request || !s_getStatus)
        return;
    auto* config = Config::Instance();
    const auto request = Control::MakeRequest(config->FGDLSSGAmpereMfgMultiplier.value_or_default(),
                                              config->FGDLSSGAmpereMfgDynamic.value_or_default(),
                                              config->FGDLSSGAmpereMfgTargetFPS.value_or_default());
    Control::RuntimeStatus status;
    s_status.ControlReady = s_getStatus(&status) != FALSE;
    if (!s_status.ControlReady)
        return;
    s_status.Runtime = status;
    if (!s_lastRequest || memcmp(&request, &*s_lastRequest, sizeof(request)) != 0)
    {
        s_status.ControlAccepted = s_request(&request) != FALSE;
        if (s_status.ControlAccepted)
        {
            s_lastRequest = request;
            LOG_INFO("SM86 control: mode {}, multiplier {}, target {} FPS", (uint32_t) request.mode, request.multiplier,
                     request.targetFps);
        }
        else if (!s_lastRefusedRequest || memcmp(&request, &*s_lastRefusedRequest, sizeof(request)) != 0)
        {
            s_lastRefusedRequest = request;
            LOG_WARN("SM86 control request was refused (Win32 error {})", GetLastError());
        }
    }
}

} // namespace AmpereMfgLoader
