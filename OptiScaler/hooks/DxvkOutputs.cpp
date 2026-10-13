#include "pch.h"
#include "DxvkOutputs.h"

#include <misc/IdentifyGpu.h>

#include <detours/detours.h>

#include <mutex>

namespace
{

using PFN_EnumOutputs = HRESULT(STDMETHODCALLTYPE*)(IDXGIAdapter* self, UINT output, IDXGIOutput** out);

PFN_EnumOutputs o_EnumOutputs = nullptr;
std::once_flag g_hookOnce;

// Where dxvk lists the primary display among this adapter's outputs; -1 when none of them is it (another GPU's).
int PrimaryIndex(IDXGIAdapter* adapter)
{
    for (UINT i = 0; i < 16; ++i)
    {
        IDXGIOutput* output = nullptr;

        if (o_EnumOutputs(adapter, i, &output) != S_OK || output == nullptr)
            return -1;

        DXGI_OUTPUT_DESC desc {};
        const bool described = SUCCEEDED(output->GetDesc(&desc));
        output->Release();

        MONITORINFO info {};
        info.cbSize = sizeof(info);

        if (described && GetMonitorInfoW(desc.Monitor, &info) && (info.dwFlags & MONITORINFOF_PRIMARY) != 0)
            return (int) i;
    }

    return -1;
}

HRESULT STDMETHODCALLTYPE hkEnumOutputs(IDXGIAdapter* self, UINT output, IDXGIOutput** out)
{
    if (out == nullptr)
        return o_EnumOutputs(self, output, out);

    const int primary = PrimaryIndex(self);

    static bool logged = false;

    if (!logged && primary > 0)
    {
        logged = true;
        LOG_INFO("dxvk lists the primary display as output {}: listing it first, as Windows' DXGI does", primary);
    }

    return o_EnumOutputs(self, DxvkOutputs::Remap(output, primary), out);
}

} // namespace

void DxvkOutputs::Hook(IUnknown* adapter)
{
    if (adapter == nullptr || o_EnumOutputs != nullptr)
        return;

    IDXGIVkInteropAdapter* dxvk = nullptr;

    if (FAILED(adapter->QueryInterface(__uuidof(IDXGIVkInteropAdapter), (void**) &dxvk)) || dxvk == nullptr)
        return;

    dxvk->Release();

    IDXGIAdapter* dxgiAdapter = nullptr;

    if (FAILED(adapter->QueryInterface(IID_PPV_ARGS(&dxgiAdapter))) || dxgiAdapter == nullptr)
        return;

    std::call_once(g_hookOnce,
                   [dxgiAdapter]
                   {
                       void** vtable = *reinterpret_cast<void***>(dxgiAdapter);
                       o_EnumOutputs = (PFN_EnumOutputs) vtable[7]; // IDXGIAdapter::EnumOutputs

                       DetourTransactionBegin();
                       DetourUpdateThread(GetCurrentThread());
                       DetourAttach(&(PVOID&) o_EnumOutputs, hkEnumOutputs);

                       if (DetourTransactionCommit() == NO_ERROR)
                       {
                           LOG_INFO("dxvk adapter: outputs listed with the primary display first");
                       }
                       else
                       {
                           LOG_ERROR("dxvk adapter: could not hook EnumOutputs");
                           o_EnumOutputs = nullptr;
                       }
                   });

    dxgiAdapter->Release();
}
