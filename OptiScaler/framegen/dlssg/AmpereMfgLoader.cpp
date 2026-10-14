#include "pch.h"

#include "AmpereMfgLoader.h"
#include "Sm86BackendAbi.h"

#include <State.h>
#include <Util.h>
#include <misc/IdentifyGpu.h>
#include <proxies/KernelBase_Proxy.h>
#include <proxies/Ntdll_Proxy.h>

#include <bcrypt.h>
#include <fstream>
#include <mutex>
#pragma comment(lib, "bcrypt.lib")

namespace AmpereMfgLoader
{
namespace
{
Status s_status;
std::recursive_mutex s_mutex;
bool s_setupAttempted = false;
HMODULE s_backend = nullptr;
HMODULE s_runtime = nullptr;
HMODULE s_plugin = nullptr;
using InfoFn = int(WINAPI*)(BackendAbi::Info*);
using InstallFn = int(WINAPI*)(const BackendAbi::Install*);
using PluginConfigFn = int(WINAPI*)(const BackendAbi::PluginConfig*);
using ImageApproxFn = int(WINAPI*)(const BackendAbi::ImageApproxConfig*);
using PluginModuleFn = void(WINAPI*)(HMODULE);
PluginModuleFn s_pluginModule = nullptr;

constexpr char kBackendSha256[] = "6cc675209e55bb3ddb6b1ba8008eef93f633d3e2207c3a073df321a6a8ed4e59";
constexpr DWORD kRuntimeBytes = 7460976;
constexpr DWORD kBackendBytes = 22066176;

bool Verified(const void* data, DWORD bytes, DWORD expectedBytes, const char* expectedSha)
{
    unsigned char hash[32] {};
    if (data == nullptr || bytes != expectedBytes ||
        BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0, (PUCHAR) data, bytes, hash, sizeof(hash)) != 0)
        return false;
    constexpr char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < sizeof(hash); ++i)
        if (expectedSha[i * 2] != hex[hash[i] >> 4] || expectedSha[i * 2 + 1] != hex[hash[i] & 15])
            return false;
    return expectedSha[64] == '\0';
}

bool VerifiedFile(const std::filesystem::path& path, DWORD bytes, const char* sha)
{
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file || file.tellg() != bytes)
        return false;
    std::vector<char> data(bytes);
    file.seekg(0);
    file.read(data.data(), data.size());
    return file && Verified(data.data(), bytes, bytes, sha);
}

void ExtractEmbedded(WORD id, const std::filesystem::path& path, DWORD expectedBytes, const char* sha)
{
    auto resource = FindResourceW(dllModule, MAKEINTRESOURCEW(id), MAKEINTRESOURCEW(10));
    auto bytes = resource ? SizeofResource(dllModule, resource) : 0;
    auto loaded = resource ? LoadResource(dllModule, resource) : nullptr;
    auto data = loaded ? LockResource(loaded) : nullptr;
    if (!Verified(data, bytes, expectedBytes, sha))
        throw std::runtime_error("Embedded SM86 component is missing or failed its pinned SHA256 check");
    if (VerifiedFile(path, expectedBytes, sha))
        return;

    std::filesystem::create_directories(path.parent_path());
    const auto temp = path.wstring() + L"." + std::to_wstring(GetCurrentProcessId()) + L".tmp";
    std::ofstream file(temp, std::ios::binary | std::ios::trunc);
    file.write((const char*) data, bytes);
    file.close();
    if (!file)
    {
        DeleteFileW(temp.c_str());
        throw std::runtime_error("Could not extract the embedded SM86 component");
    }
    if (!MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    {
        DeleteFileW(temp.c_str());
        if (!VerifiedFile(path, expectedBytes, sha))
            throw std::runtime_error("Could not install the verified SM86 component");
    }
}

template <typename T> T Resolve(HMODULE module, const char* name)
{
    return reinterpret_cast<T>(KernelBaseProxy::GetProcAddress_()(module, name));
}

void Fail(const std::string& message)
{
    s_status.ErrorMessage = message;
    LOG_ERROR("SM86 backend: {}", message);
}
} // namespace

Status LastStatus()
{
    std::lock_guard lock(s_mutex);
    return s_status;
}

bool IsReady()
{
    std::lock_guard lock(s_mutex);
    return s_status.BackendReady;
}

HMODULE RuntimeModule()
{
    std::lock_guard lock(s_mutex);
    return s_status.BackendReady ? s_runtime : nullptr;
}

bool EnsureSetup()
{
    // External FG has its own runtime and presentation owner. Do not patch its
    // provider, even during hardware discovery or before local Streamline loads.
    if (State::Instance().externalFrameGeneration)
        return false;

    // Discovery can load graphics libraries and re-enter OptiScaler. Never hold
    // the adapter lock, or consume its one setup attempt, during that discovery.
    // A recursive load on this thread must not start hardware discovery again.
    static thread_local bool entering = false;
    if (entering)
        return false;
    struct EntryGuard
    {
        bool& value;
        explicit EntryGuard(bool& flag) : value(flag) { value = true; }
        ~EntryGuard() { value = false; }
    } guard(entering);
    const auto gpu = IdentifyGpu::getPrimaryGpu();
    if (gpu.vendorId == VendorId::Invalid)
        return false;

    std::lock_guard lock(s_mutex);
    if (s_setupAttempted)
        return s_status.BackendReady;

    const auto arch = static_cast<uint32_t>(gpu.nvidiaArchInfo.architecture_id);
    const bool supported =
        gpu.vendorId == VendorId::Nvidia &&
        (IsAmpereArch(arch) || IsTuringArch(arch) || gpu.name.find("RTX 30") != std::string::npos ||
         gpu.name.find("RTX 20") != std::string::npos || gpu.name.find("GTX 16") != std::string::npos);
    if (!supported)
        return false;
    s_setupAttempted = true;
    s_status.Enabled = true;

    // A separately installed proxy would patch the same runtime independently.
    // The embedded adapter itself never loads either of the upstream proxies.
    for (const wchar_t* name : { L"version.dll", L"dlssg_sm86.dll", L"dinput8.dll", L"dxgi.dll", L"winmm.dll" })
    {
        auto module = GetModuleHandleW(name);
        if (module != nullptr && module != dllModule &&
            (Resolve<FARPROC>(module, "DLSSG_UniversalProxy") != nullptr ||
             Resolve<FARPROC>(module, "DlssgProxy_Name") != nullptr))
        {
            Fail("A separate SM86 proxy is loaded. Remove it and restart to use OptiScaler's backend.");
            return false;
        }
    }

    const auto directory = Util::DllPath().parent_path() / L"OptiScaler" / L"dlssg_sm86";
    const auto runtimePath = directory / L"nvngx_dlssg.dll";
    const auto backendPath = directory / L"sm86_backend.dll";
    try
    {
        ExtractEmbedded(31011, runtimePath, kRuntimeBytes, BackendAbi::kRuntimeSha256);
        ExtractEmbedded(31012, backendPath, kBackendBytes, kBackendSha256);
        s_status.Embedded = true;
        s_status.DllFound = true;
    }
    catch (const std::exception& error)
    {
        Fail(error.what());
        return false;
    }

    NtdllProxy::Init();
    auto owner = State::GetOwner();
    State::DisableChecks(owner);
    s_backend = NtdllProxy::LoadLibraryExW_Ldr(backendPath.c_str(), nullptr, 0);
    s_runtime = s_backend ? NtdllProxy::LoadLibraryExW_Ldr(runtimePath.c_str(), nullptr, 0) : nullptr;
    const auto loadError = GetLastError();
    State::EnableChecks(owner);
    if (s_backend == nullptr || s_runtime == nullptr)
    {
        Fail(std::format("Could not map the embedded components (Win32 error {})", loadError));
        return false;
    }
    s_status.DllLoaded = true;

    auto infoFn = Resolve<InfoFn>(s_backend, "DlssgMod_GetInfo");
    auto installFn = Resolve<InstallFn>(s_backend, "DlssgMod_Install");
    auto pluginConfigFn = Resolve<PluginConfigFn>(s_backend, "DlssgMod_ConfigurePlugin");
    auto imageApproxFn = Resolve<ImageApproxFn>(s_backend, "DlssgMod_ConfigureImageApprox");
    s_pluginModule = Resolve<PluginModuleFn>(s_backend, "DlssgMod_PluginModule");
    BackendAbi::Info info;
    if (!infoFn || !installFn || !pluginConfigFn || !imageApproxFn || !s_pluginModule || infoFn(&info) != 0 ||
        info.size != sizeof(info) || info.version != 1 || info.runtimeSha256 == nullptr ||
        strcmp(info.runtimeSha256, BackendAbi::kRuntimeSha256) != 0 ||
        info.maxGeneratedFrames < BackendAbi::kMaxGeneratedFrames)
    {
        Fail("The pinned backend's versioned ABI or NVIDIA runtime contract was rejected");
        return false;
    }

    BackendAbi::PluginConfig pluginConfig;
    BackendAbi::ImageApproxConfig imageApprox;
    if (pluginConfigFn(&pluginConfig) != 0 || imageApproxFn(&imageApprox) != 0)
    {
        Fail("The backend rejected its native plugin capability or default optimization settings");
        return false;
    }
    BackendAbi::Install install;
    install.runtime = s_runtime;
    const auto result = installFn(&install);
    if (result != 0)
    {
        Fail(std::format("The backend could not initialize NVIDIA DLSS-G 310.9.1 (result {})", result));
        return false;
    }

    s_status.MaxGeneratedFrames = BackendAbi::kMaxGeneratedFrames;
    s_status.BackendName = info.name ? info.name : "SM86";
    s_status.BackendReady = true;
    s_status.ErrorMessage.clear();
    LOG_INFO("SM86 backend {} ready for {}; OptiScaler owns Streamline options and presentation", s_status.BackendName,
             gpu.name);
    return true;
}

bool ConfigurePlugin(HMODULE plugin)
{
    std::lock_guard lock(s_mutex);
    if (!s_status.BackendReady || plugin == nullptr || s_pluginModule == nullptr)
        return false;
    if (s_plugin != nullptr && s_plugin != plugin)
    {
        Fail("Refusing to bind a second DLSS-G plugin to the backend");
        return false;
    }
    if (s_plugin == nullptr)
    {
        s_pluginModule(plugin);
        s_plugin = plugin;
        s_status.PluginBound = true;
        LOG_INFO("SM86 backend bound to OptiScaler DLSS-G plugin {:X}", (uintptr_t) plugin);
    }
    return true;
}

void TrySetup() { EnsureSetup(); }
void UpdateControls() {}

std::string ResolveAutoKernelImage()
{
    const auto gpu = IdentifyGpu::getPrimaryGpu();
    return ResolveAutoKernelImage(static_cast<uint32_t>(gpu.nvidiaArchInfo.architecture_id), gpu.name,
                                  State::Instance().isRunningOnLinux || gpu.d3d12IsVkd3dProton);
}

std::string ResolveRouter()
{
    const auto gpu = IdentifyGpu::getPrimaryGpu();
    return ResolveRouter(static_cast<uint32_t>(gpu.nvidiaArchInfo.architecture_id), gpu.name);
}

std::string GenerateIniContent()
{
    // Legacy helper only. The direct adapter passes RuntimeConfig through its
    // versioned ABI and never creates or consumes a companion configuration.
    return FormatIniContent(3, ResolveAutoKernelImage(), 0, ResolveRouter(), 0);
}
} // namespace AmpereMfgLoader
