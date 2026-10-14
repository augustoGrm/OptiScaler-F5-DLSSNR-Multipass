#pragma once

namespace DlssgLifetime
{
// The probe keeps the object alive while the last caller's resources are
// detached. It is not an extra reference that may be kept after this call.
// Streamline owns its inner swapchain and buffers; only its destructor may
// release those references.
template <class Swapchain, class Release, class BeforeDestroy>
auto ReleaseReference(Swapchain* swapchain, Release release, BeforeDestroy beforeDestroy)
{
    swapchain->AddRef();
    const auto remaining = release(swapchain);
    if (remaining == 1)
        beforeDestroy();
    return release(swapchain);
}
} // namespace DlssgLifetime
