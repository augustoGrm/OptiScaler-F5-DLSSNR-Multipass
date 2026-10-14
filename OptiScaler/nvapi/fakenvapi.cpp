#include "pch.h"

#include "proxies/FfxApi_Proxy.h"
#include <unordered_map>

#include "fakenvapi.h"
#include "NvApiTypes.h"
#include "fakenvapi/nvapi_calls.h"

#define nvapi_interface_table nvapi_interface_table_extern
#include <nvapi_interface.h>
#undef nvapi_interface_table

std::unordered_map<NvU32, void*> fakenvapi::idToFuncMapping;

void fakenvapi::init(bool onlyContext)
{
    LowLatencyCtx::init();

    if (onlyContext)
        return;
}

// The second context XeFG's Anti-Lag 2 output runs in (fakenvapi.h companion*): one for D3D, one for Vulkan, each its
// own LowLatency with one tech. The mutex only guards making, switching and dropping the context: a call copies the
// pointer under it and runs outside, because Sleep and the simulation-start marker block inside Anti-Lag 2's Update and
// the render thread's markers must not wait for the game thread's sleep.
namespace
{
struct Companion
{
    std::mutex mutex;
    std::shared_ptr<LowLatency> context;
    const void* device = nullptr;
    LowLatencyMode tech = LowLatencyMode::None;
};

Companion g_companionD3D { {}, nullptr, nullptr, LowLatencyMode::AntiLag2 };
Companion g_companionVk { {}, nullptr, nullptr, LowLatencyMode::AntiLagVk };

// Set once a tech could not start: kept after the context is let go, or the Output would flip back and forth
std::atomic<bool> g_companionFailed = false;

// The context for `device`: made on the first call, its tech made again for a new device. `fn` runs outside the lock.
template <typename Fn> NvAPI_Status WithCompanion(Companion& companion, const void* device, Fn&& fn)
{
    std::shared_ptr<LowLatency> context;

    {
        std::scoped_lock lock(companion.mutex);

        if (companion.context == nullptr)
        {
            companion.context = std::make_shared<LowLatency>();
            companion.context->use_only(companion.tech);
            companion.device = device;
        }
        else if (companion.device != device)
        {
            companion.context->use_only(companion.tech);
            companion.device = device;
        }

        context = companion.context;
    }

    const auto result = fn(*context);

    if (context->init_failed())
        g_companionFailed = true;

    return result;
}

void DropCompanion(Companion& companion)
{
    std::shared_ptr<LowLatency> context;

    {
        std::scoped_lock lock(companion.mutex);

        context = std::move(companion.context);
        companion.device = nullptr;
    }

    // Let go outside the lock: the last reference waits for a call still inside its tech
}
} // namespace

NvAPI_Status fakenvapi::companionSetSleepMode(IUnknown* device, NV_SET_SLEEP_MODE_PARAMS* params)
{
    return WithCompanion(g_companionD3D, device, [&](LowLatency& ll) { return ll.SetSleepMode(device, params); });
}

NvAPI_Status fakenvapi::companionSleep(IUnknown* device)
{
    return WithCompanion(g_companionD3D, device, [&](LowLatency& ll) { return ll.Sleep(device); });
}

NvAPI_Status fakenvapi::companionSetLatencyMarker(IUnknown* device, NV_LATENCY_MARKER_PARAMS* params)
{
    return WithCompanion(g_companionD3D, device, [&](LowLatency& ll) { return ll.SetLatencyMarker(device, params); });
}

NvAPI_Status fakenvapi::companionSetAsyncFrameMarker(ID3D12CommandQueue* queue, NV_ASYNC_FRAME_MARKER_PARAMS* params)
{
    std::shared_ptr<LowLatency> context;

    {
        std::scoped_lock lock(g_companionD3D.mutex);
        context = g_companionD3D.context;
    }

    // It cannot start its tech from a queue: only once a device call has
    if (context == nullptr)
        return NVAPI_ERROR;

    return context->SetAsyncFrameMarker(queue, params);
}

NvAPI_Status fakenvapi::companionGetLatency(IUnknown* device, NV_LATENCY_RESULT_PARAMS* params)
{
    return WithCompanion(g_companionD3D, device, [&](LowLatency& ll) { return ll.GetLatency(device, params); });
}

NvAPI_Status fakenvapi::companionSetSleepMode(HANDLE vkDevice, NV_VULKAN_SET_SLEEP_MODE_PARAMS* params)
{
    return WithCompanion(g_companionVk, vkDevice, [&](LowLatency& ll) { return ll.SetSleepMode(vkDevice, params); });
}

NvAPI_Status fakenvapi::companionSetLatencyMarker(HANDLE vkDevice, NV_VULKAN_LATENCY_MARKER_PARAMS* params)
{
    return WithCompanion(g_companionVk, vkDevice,
                         [&](LowLatency& ll) { return ll.SetLatencyMarker(vkDevice, params); });
}

NvAPI_Status fakenvapi::companionGetLatency(HANDLE vkDevice, NV_VULKAN_LATENCY_RESULT_PARAMS* params)
{
    return WithCompanion(g_companionVk, vkDevice, [&](LowLatency& ll) { return ll.GetLatency(vkDevice, params); });
}

void fakenvapi::companionDeviceGone(const void* device)
{
    for (Companion* companion : { &g_companionD3D, &g_companionVk })
    {
        std::shared_ptr<LowLatency> context;

        {
            std::scoped_lock lock(companion->mutex);

            if (companion->context != nullptr && companion->device == device)
            {
                context = companion->context;
                companion->device = nullptr;
            }
        }

        // Its tech goes with the device (outside the lock: it waits for a call still inside); the next call with a new
        // device makes it again
        if (context != nullptr)
            context->use_only(companion->tech);
    }
}

bool fakenvapi::companionFailed() { return g_companionFailed.load(); }

void fakenvapi::companionRelease()
{
    DropCompanion(g_companionD3D);
    DropCompanion(g_companionVk);
}

void fakenvapi::deinit()
{
    DropCompanion(g_companionD3D);
    DropCompanion(g_companionVk);

    LowLatencyCtx::get()->deinit_current_tech();
    LowLatencyCtx::shutdown();
}

// names from: https://github.com/SveSop/nvapi_standalone/blob/master/dlls/nvapi/nvapi.c
static NVAPI_INTERFACE_TABLE additional_interface_table[] = { { "NvAPI_Diag_ReportCallStart", 0x33c7358c },
                                                              { "NvAPI_Diag_ReportCallReturn", 0x593e8644 },
                                                              { "NvAPI_Unknown_1", 0xe9b009b9 },
                                                              { "NvAPI_SK_1", 0x57f7caac },
                                                              { "NvAPI_SK_2", 0x11104158 },
                                                              { "NvAPI_SK_3", 0xe3795199 },
                                                              { "NvAPI_SK_4", 0xdf0dfcdd },
                                                              { "NvAPI_SK_5", 0x932ac8fb } };

extern "C" __declspec(dllexport) void* nvapi_QueryInterface(NvU32 id) { return fakenvapi::queryInterface(id); }

void* __cdecl fakenvapi::queryInterface(NvU32 id)
{
    auto entry = idToFuncMapping.find(id);
    if (entry != idToFuncMapping.end())
        return entry->second;

    constexpr auto original_size = sizeof(nvapi_interface_table_extern) / sizeof(nvapi_interface_table_extern[0]);
    constexpr auto additional_size = sizeof(additional_interface_table) / sizeof(additional_interface_table[0]);

    constexpr auto total_size = original_size + additional_size;

    struct NVAPI_INTERFACE_TABLE extended_interface_table[total_size] {};
    memcpy(extended_interface_table, nvapi_interface_table_extern,
           sizeof(nvapi_interface_table_extern)); // copy original table

    for (unsigned int i = 0; i < additional_size; i++)
    {
        extended_interface_table[original_size + i] = additional_interface_table[i];
    }

    auto it = std::find_if(std::begin(extended_interface_table), std::end(extended_interface_table),
                           [id](const auto& item) { return item.id == id; });

    if (it == std::end(extended_interface_table))
    {
        LOG_DEBUG("NvAPI_QueryInterface (0x{:x}): Unknown interface ID", id);
        return idToFuncMapping.insert({ id, nullptr }).first->second;
    }

    INSERT_AND_RETURN_WHEN_EQUALS(NvAPI_Initialize)
    INSERT_AND_RETURN_WHEN_EQUALS(NvAPI_GetInterfaceVersionString)
    INSERT_AND_RETURN_WHEN_EQUALS(NvAPI_EnumNvidiaDisplayHandle)
    INSERT_AND_RETURN_WHEN_EQUALS(NvAPI_GetLogicalGPUFromPhysicalGPU)
    INSERT_AND_RETURN_WHEN_EQUALS(NvAPI_EnumPhysicalGPUs)
    INSERT_AND_RETURN_WHEN_EQUALS(NvAPI_EnumLogicalGPUs)
    INSERT_AND_RETURN_WHEN_EQUALS(NvAPI_GetGPUIDfromPhysicalGPU)
    INSERT_AND_RETURN_WHEN_EQUALS(NvAPI_GetPhysicalGPUFromGPUID)
    INSERT_AND_RETURN_WHEN_EQUALS(NvAPI_GetPhysicalGPUsFromDisplay)
    INSERT_AND_RETURN_WHEN_EQUALS(NvAPI_GetPhysicalGPUsFromLogicalGPU)
    INSERT_AND_RETURN_WHEN_EQUALS(NvAPI_GetErrorMessage)
    INSERT_AND_RETURN_WHEN_EQUALS(NvAPI_GetDisplayDriverVersion)
    INSERT_AND_RETURN_WHEN_EQUALS(NvAPI_GPU_GetLogicalGpuInfo)
    INSERT_AND_RETURN_WHEN_EQUALS(NvAPI_GPU_GetConnectedDisplayIds)
    INSERT_AND_RETURN_WHEN_EQUALS(NvAPI_GPU_CudaEnumComputeCapableGpus)
    INSERT_AND_RETURN_WHEN_EQUALS(NvAPI_GPU_GetArchInfo)
    INSERT_AND_RETURN_WHEN_EQUALS(NvAPI_GPU_GetPCIIdentifiers)
    INSERT_AND_RETURN_WHEN_EQUALS(NvAPI_GPU_GetFullName)
    INSERT_AND_RETURN_WHEN_EQUALS(NvAPI_GPU_GetGpuCoreCount)
    INSERT_AND_RETURN_WHEN_EQUALS(NvAPI_GPU_GetAllClockFrequencies)
    INSERT_AND_RETURN_WHEN_EQUALS(NvAPI_GPU_GetAdapterIdFromPhysicalGpu)
    INSERT_AND_RETURN_WHEN_EQUALS(NvAPI_GPU_GetPstates20)
    INSERT_AND_RETURN_WHEN_EQUALS(NvAPI_DISP_GetDisplayIdByDisplayName)
    INSERT_AND_RETURN_WHEN_EQUALS(NvAPI_DISP_GetGDIPrimaryDisplayId)
    INSERT_AND_RETURN_WHEN_EQUALS(NvAPI_Disp_SetOutputMode)
    INSERT_AND_RETURN_WHEN_EQUALS(NvAPI_Disp_GetOutputMode)
    INSERT_AND_RETURN_WHEN_EQUALS(NvAPI_Disp_GetHdrCapabilities)
    INSERT_AND_RETURN_WHEN_EQUALS(NvAPI_Disp_HdrColorControl)
    INSERT_AND_RETURN_WHEN_EQUALS(NvAPI_Mosaic_GetDisplayViewportsByResolution)
    INSERT_AND_RETURN_WHEN_EQUALS(NvAPI_SYS_GetDisplayDriverInfo)
    INSERT_AND_RETURN_WHEN_EQUALS(NvAPI_SYS_GetDriverAndBranchVersion)
    INSERT_AND_RETURN_WHEN_EQUALS(NvAPI_SYS_GetDisplayIdFromGpuAndOutputId)
    INSERT_AND_RETURN_WHEN_EQUALS(NvAPI_SYS_GetGpuAndOutputIdFromDisplayId)
    INSERT_AND_RETURN_WHEN_EQUALS(NvAPI_D3D_SetResourceHint)
    INSERT_AND_RETURN_WHEN_EQUALS(NvAPI_D3D_GetObjectHandleForResource)
    INSERT_AND_RETURN_WHEN_EQUALS(NvAPI_D3D_GetSleepStatus)
    INSERT_AND_RETURN_WHEN_EQUALS(NvAPI_D3D_GetLatency)
    INSERT_AND_RETURN_WHEN_EQUALS(NvAPI_D3D_SetSleepMode)
    INSERT_AND_RETURN_WHEN_EQUALS(NvAPI_D3D_SetLatencyMarker)
    INSERT_AND_RETURN_WHEN_EQUALS(NvAPI_D3D_Sleep)
    INSERT_AND_RETURN_WHEN_EQUALS(NvAPI_D3D_SetReflexSync)
    INSERT_AND_RETURN_WHEN_EQUALS(NvAPI_D3D11_IsNvShaderExtnOpCodeSupported)
    INSERT_AND_RETURN_WHEN_EQUALS(NvAPI_D3D11_BeginUAVOverlap)
    INSERT_AND_RETURN_WHEN_EQUALS(NvAPI_D3D11_EndUAVOverlap)
    INSERT_AND_RETURN_WHEN_EQUALS(NvAPI_D3D11_SetDepthBoundsTest)
    INSERT_AND_RETURN_WHEN_EQUALS(NvAPI_D3D12_GetRaytracingCaps)
    INSERT_AND_RETURN_WHEN_EQUALS(NvAPI_D3D12_IsNvShaderExtnOpCodeSupported)
    INSERT_AND_RETURN_WHEN_EQUALS(NvAPI_D3D12_SetNvShaderExtnSlotSpaceLocalThread)
    INSERT_AND_RETURN_WHEN_EQUALS(NvAPI_D3D12_GetRaytracingAccelerationStructurePrebuildInfoEx)
    INSERT_AND_RETURN_WHEN_EQUALS(NvAPI_D3D12_BuildRaytracingAccelerationStructureEx)
    INSERT_AND_RETURN_WHEN_EQUALS(NvAPI_D3D12_NotifyOutOfBandCommandQueue)
    INSERT_AND_RETURN_WHEN_EQUALS(NvAPI_D3D12_SetAsyncFrameMarker)
    INSERT_AND_RETURN_WHEN_EQUALS(NvAPI_Vulkan_InitLowLatencyDevice)
    INSERT_AND_RETURN_WHEN_EQUALS(NvAPI_Vulkan_DestroyLowLatencyDevice)
    INSERT_AND_RETURN_WHEN_EQUALS(NvAPI_Vulkan_GetSleepStatus)
    INSERT_AND_RETURN_WHEN_EQUALS(NvAPI_Vulkan_SetSleepMode)
    INSERT_AND_RETURN_WHEN_EQUALS(NvAPI_Vulkan_Sleep)
    INSERT_AND_RETURN_WHEN_EQUALS(NvAPI_Vulkan_GetLatency)
    INSERT_AND_RETURN_WHEN_EQUALS(NvAPI_Vulkan_SetLatencyMarker)
    INSERT_AND_RETURN_WHEN_EQUALS(NvAPI_Vulkan_NotifyOutOfBandVkQueue)
    INSERT_AND_RETURN_WHEN_EQUALS(NvAPI_DRS_CreateSession)
    INSERT_AND_RETURN_WHEN_EQUALS(NvAPI_DRS_LoadSettings)
    INSERT_AND_RETURN_WHEN_EQUALS(NvAPI_DRS_SaveSettings)
    INSERT_AND_RETURN_WHEN_EQUALS(NvAPI_DRS_GetBaseProfile)
    INSERT_AND_RETURN_WHEN_EQUALS(NvAPI_DRS_GetSetting)
    INSERT_AND_RETURN_WHEN_EQUALS(NvAPI_DRS_SetSetting)
    INSERT_AND_RETURN_WHEN_EQUALS(NvAPI_DRS_DestroySession)
    INSERT_AND_RETURN_WHEN_EQUALS(NvAPI_NGX_GetDriverFeatureSupport)
    INSERT_AND_RETURN_WHEN_EQUALS(NvAPI_Unknown_1)
    INSERT_AND_RETURN_WHEN_EQUALS(NvAPI_SK_1)
    INSERT_AND_RETURN_WHEN_EQUALS(NvAPI_SK_2)
    INSERT_AND_RETURN_WHEN_EQUALS(NvAPI_SK_3)
    INSERT_AND_RETURN_WHEN_EQUALS(NvAPI_SK_4)
    INSERT_AND_RETURN_WHEN_EQUALS(NvAPI_SK_5)
    INSERT_AND_RETURN_WHEN_EQUALS(NvAPI_Unload)

    LOG_DEBUG("{}: not implemented, placeholder given", it->func);
    return idToFuncMapping.insert({ id, (void*) placeholder }).first->second;
    // return registry.insert({ id, nullptr }).first->second;
}

// Inform AntiLag 2 when present of interpolated frames starts
void fakenvapi::reportFGPresent(IDXGISwapChain* pSwapChain, bool fg_state, bool frame_interpolated)
{
    if (!isUsingAsMainNvapi() || State::Instance().activeFgOutput != FGOutput::FSRFG)
        return;

    auto lowLatencyCtx = LowLatencyCtx::get();

    // Lets fakenvapi log and reset correctly
    if (lowLatencyCtx)
        lowLatencyCtx->set_forced_fg(fg_state);
    else
        LOG_ERROR("Couldn't get low latency context");

    if (fg_state)
    {
        // Starting with FSR 3.1.1 we can provide an AntiLag 2 context to FSR FG
        // and it will call SetFrameGenFrameType for us
        auto static ffxApiVersion = FfxApiProxy::VersionDx12();
        constexpr feature_version requiredVersion = { 3, 1, 1 };
        if (ffxApiVersion >= requiredVersion && updateModeAndContext())
        {
            antilag2_data.enabled = _lowLatencyTechContext != nullptr && _lowLatencyMode == LowLatencyMode::AntiLag2;
            antilag2_data.context = antilag2_data.enabled ? _lowLatencyTechContext : nullptr;

            pSwapChain->SetPrivateData(IID_IFfxAntiLag2Data, sizeof(antilag2_data), &antilag2_data);
        }
        else
        {
            // Tell fakenvapi to call SetFrameGenFrameType by itself
            // Reflex frame id might get used in the future
            LOG_TRACE("Fake_InformPresentFG: {}", frame_interpolated);

            if (lowLatencyCtx)
                lowLatencyCtx->set_fg_type(frame_interpolated, 0);
        }
    }
    else
    {
        // Remove it or other FG mods won't work with AL2
        pSwapChain->SetPrivateData(IID_IFfxAntiLag2Data, 0, nullptr);
    }
}

bool fakenvapi::updateModeAndContext()
{
    LOG_FUNC();

    auto lowLatencyCtx = LowLatencyCtx::get();

    if (!lowLatencyCtx)
    {
        // Real reflex should fall here and not log
        if (_usingFakenvapiAsMainNvapi)
            LOG_ERROR("Couldn't get low latency context");

        return false;
    }

    auto result = lowLatencyCtx->get_low_latency_tech_context(&_lowLatencyTechContext, &_lowLatencyMode);

    if (!result)
        LOG_TRACE_FAKENVAPI("Can't get Low Latency context from fakenvapi");

    return result;
}

bool fakenvapi::forceMode(IUnknown* device, LowLatencyMode mode)
{
    LOG_FUNC();

    // Force init here, in case Reflex is not supported
    LowLatencyCtx::init();

    auto lowLatencyCtx = LowLatencyCtx::get();

    if (!lowLatencyCtx)
    {
        LOG_ERROR("Couldn't get low latency context");
        return false;
    }

    auto result = lowLatencyCtx->set_low_latency_tech_mode(device, mode);
    ++_forcedModeGeneration;

    if (!result)
        LOG_TRACE_FAKENVAPI("Can't set Low Latency context from fakenvapi");

    return result;
}

bool fakenvapi::isLowLatencyActive()
{
    auto lowLatencyCtx = LowLatencyCtx::get();

    if (!lowLatencyCtx)
    {
        if (_usingFakenvapiAsMainNvapi)
            LOG_ERROR("Couldn't get low latency context");

        return false;
    }

    return lowLatencyCtx->is_low_latency_enabled();
}

uint32_t fakenvapi::forcedModeGeneration() { return _forcedModeGeneration.load(); }

LowLatencyMode fakenvapi::getCurrentMode()
{
    if (updateModeAndContext())
        return _lowLatencyMode;
    else
        return LowLatencyMode::None;
}

void* fakenvapi::getCurrentContext()
{
    if (updateModeAndContext())
        return _lowLatencyTechContext;
    else
        return nullptr;
}

bool fakenvapi::isUsingAsMainNvapi() { return _usingFakenvapiAsMainNvapi; }

void fakenvapi::setUsingAsMainNvapi(bool usingAsMain) { _usingFakenvapiAsMainNvapi = usingAsMain; }
