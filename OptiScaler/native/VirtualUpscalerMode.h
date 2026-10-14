#pragma once

// What Optical F5Low's virtual upscaler (native/VirtualUpscalerDriver.h) does with Neural Rendering on a frame. Pure, so a
// host test can run it (tests/f5low_vu_route_smoke.cpp).
//
// The backend (DLSS, FSR, XeSS) exists so frame generation has an upscaler feature to take motion and depth from. Its pass
// over the picture is not needed for that: the picture is already full size and antialiased. With Neural Rendering on, NR
// therefore runs on the picture in place, as in "NR only" mode (HDR10 included: that entry converts it itself), and the
// backend is only counted as a frame. The ini switch can ask for the backend's pass back (to compare, or as a fallback).

namespace native
{

// [DlssNr] NativeUpscalerPass: unset = Auto (no pass), false = Off (no pass), true = On (the backend's pass).
enum class VuPassSetting
{
    Auto,
    Off,
    On
};

enum class VuNrRoute
{
    None,        // Neural Rendering is off: the picture goes on as the game made it
    InPlace,     // NR runs on the picture in place; the backend is only counted
    BackendPass, // the backend evaluates and NR runs on its output, which is copied back over the picture
};

inline VuPassSetting VuPassSettingFromIni(bool hasValue, bool value)
{
    return !hasValue ? VuPassSetting::Auto : value ? VuPassSetting::On : VuPassSetting::Off;
}

inline VuNrRoute DecideVuNrRoute(bool nrEnabled, VuPassSetting setting)
{
    if (!nrEnabled)
        return VuNrRoute::None;

    if (setting == VuPassSetting::On)
        return VuNrRoute::BackendPass;

    return VuNrRoute::InPlace;
}

} // namespace native
