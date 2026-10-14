#include "../OptiScaler/framegen/dlssg/SwapchainLifetime.h"

#include <cassert>
#include <cstdio>

struct NativeSwapchain
{
    unsigned references = 1;
    bool& windowInUse;
    bool destroyed = false;

    explicit NativeSwapchain(bool& occupied) : windowInUse(occupied)
    {
        assert(!windowInUse);
        windowInUse = true;
    }
    void AddRef()
    {
        assert(!destroyed);
        ++references;
    }
    unsigned Release()
    {
        assert(!destroyed && references > 0);
        if (--references == 0)
        {
            windowInUse = false;
            destroyed = true;
        }
        return references;
    }
};

struct StreamlineSwapchain
{
    unsigned references = 1;
    NativeSwapchain& native;
    bool destroyed = false;
    bool detached = false;

    void AddRef()
    {
        assert(!destroyed);
        ++references;
    }
    unsigned Release()
    {
        assert(!destroyed && references > 0);
        if (--references == 0)
        {
            assert(detached);
            destroyed = true;
            native.Release(); // Only Streamline owns this reference.
        }
        return references;
    }
};

static unsigned ReleaseOriginal(StreamlineSwapchain* swapchain) { return swapchain->Release(); }

int main()
{
    // The actual failure: the first swapchain must release its HWND before a
    // game can recreate it on the same window. A retained probe leaves it busy.
    bool windowInUse = false;
    for (int recreation = 0; recreation < 5; ++recreation)
    {
        NativeSwapchain native(windowInUse);
        StreamlineSwapchain streamline { 1, native };
        int cleanupCalls = 0;
        auto beforeDestroy = [&]
        {
            assert(!streamline.destroyed && native.references == 1 && windowInUse);
            ++cleanupCalls;
            streamline.detached = true;
        };

        // An overlay and a temporary QueryInterface each hold a reference.
        streamline.AddRef();
        streamline.AddRef();
        assert(DlssgLifetime::ReleaseReference(&streamline, ReleaseOriginal, beforeDestroy) == 2);
        assert(cleanupCalls == 0 && !native.destroyed);
        assert(DlssgLifetime::ReleaseReference(&streamline, ReleaseOriginal, beforeDestroy) == 1);
        assert(cleanupCalls == 0 && !native.destroyed);
        assert(DlssgLifetime::ReleaseReference(&streamline, ReleaseOriginal, beforeDestroy) == 0);
        assert(cleanupCalls == 1 && streamline.destroyed && native.destroyed && !windowInUse);
    }

    // A foreign component's native reference must survive Streamline teardown.
    NativeSwapchain native(windowInUse);
    native.AddRef();
    StreamlineSwapchain streamline { 1, native };
    assert(DlssgLifetime::ReleaseReference(&streamline, ReleaseOriginal, [&] { streamline.detached = true; }) == 0);
    assert(streamline.destroyed && !native.destroyed && native.references == 1 && windowInUse);
    assert(native.Release() == 0 && !windowInUse);
    std::puts("DLSSG last-reference cleanup, shared ownership and same-window recreation passed.");
}
