#pragma once

// The menu's look presets: one click sets everything about how DLSS-NR's edit looks. Plain data and rules, no config
// and no ImGui, so tests/nr_look_presets_smoke.cpp runs them; dlssnr/DlssNr_Menu.cpp reads the config into a Settings
// and writes one back.
//
// Each preset row on the menu owns its own settings, and its green highlight checks only those, so the rows never undo
// each other:
// - a look owns the per-pass model profiles, the curve, Detail and Colour strength, the skin / environment split and
//   the input style (Settings below);
// - the Multipass presets own the pass count; the Pre-SR / Post-SR tiers own placement and model resolution.
// A look never sets exposure, colour encoding or paper white: those belong to the game and the display, not to taste
// (Native look's brightness suits NBA 2K27 and is far too dark in RDR2).

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace DlssNrLook
{
// [DlssNr] ReversibleMode, the curve and how the answer is put back (Config.h)
inline constexpr uint32_t kCurveSoftKnee = 0;        // soft knee + composed (the default)
inline constexpr uint32_t kCurveComposed = 1;        // unclipped proxy + composed: the tiers' composition
inline constexpr uint32_t kCurveReplace = 2;         // pure-inverse replace
inline constexpr uint32_t kCurveReplaceBalanced = 4; // balanced curve, replace
inline constexpr uint32_t kCurveHlg = 5;             // HLG + composed
inline constexpr uint32_t kCurveLinear = 7;          // linear + composed: what game integrations hand the model

// [DlssNr] InputStyle
inline constexpr uint32_t kInputOff = 0;
inline constexpr uint32_t kInputWatercolour = 1;
inline constexpr uint32_t kInputSmoothCanvas = 3;

inline bool IsReplace(uint32_t curve) { return curve == kCurveReplace || curve == kCurveReplaceBalanced; }

// What a tier leaves the curve at: composed always (Replace was the lower tiers' and is no longer), and a composed
// curve a look chose stays, so the look stays in effect after a tier.
inline uint32_t TierCurve(uint32_t current) { return IsReplace(current) ? kCurveComposed : current; }

// One pass's model profile ([DlssNr] Style, Intensity, LocalStructure, LocalTone, SkinStructure, AutoMask)
struct Profile
{
    uint32_t style = 0; // 0 Standard, 1 Natural, 2 Cinematic
    float intensity = 1.0f;
    float structure = 1.0f;
    float tone = 1.0f;
    float skin = -1.0f; // -1: follow Local structure
    bool autoMask = true;
};

// Everything a look owns. Values a look does not mention are the defaults below, so a look gives the same picture
// whatever was set before it.
struct Settings
{
    Profile pass1;
    std::optional<Profile> pass2; // absent: inherits pass 1 (Local tone 0 from pass 2 on, as PassProfiles.h has it)
    std::optional<Profile> pass3;
    bool laterOverrides = false; // any pass 4+ override set (a look sets none)
    uint32_t curve = kCurveSoftKnee;
    float detail = 1.0f;    // TransferStrength, the menu's Detail strength
    float colour = 1.0f;    // ColourStrength
    bool skinSplit = false; // SkinProtection: separate skin / environment controls
    bool skinTone = true;   // SkinToneEnabled
    float skinDetail = 1.0f;
    float skinColour = 1.0f;
    float envDetail = 1.0f;
    float envColour = 1.0f;
    uint32_t inputStyle = kInputOff;
    float inputStrength = 1.0f;
    bool facesOnly = false;
};

enum class Look
{
    Native,
    Natural,
    Cinematic,
    DetailOnly,
    Faces,
    Painterly,
    SoftCanvas,
};

struct Entry
{
    Look id;
    const char* name;
    const char* tip;
    bool experimental; // an input style: no effect above 100% model resolution or on the deferred path
    Settings settings;
};

namespace detail
{
inline Settings Make(Profile pass1, uint32_t curve)
{
    Settings s;
    s.pass1 = pass1;
    s.curve = curve;
    return s;
}

inline Settings Native() { return Make({ 0, 1.0f, 1.0f, 0.0f, 0.75f, true }, kCurveLinear); }

// The tuned multipass profiles (were the Multipass presets'), pass 1 toned down from 1.8 to 1.4 (user, 2026-10-14)
inline Settings Natural()
{
    Settings s = Make({ 1, 1.4f, 1.4f, 1.4f, -1.0f, true }, kCurveHlg);
    s.pass2 = Profile { 1, 1.0f, 1.0f, 1.0f, -1.0f, true };
    s.pass3 = Profile { 1, 0.75f, 1.5f, 0.46f, 1.0f, true };
    return s;
}

inline Settings Cinematic() { return Make({ 2, 1.2f, 1.0f, 1.0f, -1.0f, true }, kCurveSoftKnee); }

// Colour strength 0: only brightness detail reaches the picture, so no hue can move
inline Settings DetailOnly()
{
    Settings s = Make({ 0, 0.8f, 1.0f, 1.0f, -1.0f, true }, kCurveSoftKnee);
    s.colour = 0.0f;
    return s;
}

inline Settings Faces()
{
    Settings s = Make({ 0, 1.0f, 1.0f, 1.0f, 1.0f, true }, kCurveSoftKnee);
    s.skinSplit = true;
    s.envDetail = 0.6f;
    return s;
}

inline Settings Painterly()
{
    Settings s = Make({ 0, 1.0f, 1.0f, 1.0f, -1.0f, true }, kCurveSoftKnee);
    s.inputStyle = kInputWatercolour;
    return s;
}

inline Settings SoftCanvas()
{
    Settings s = Make({ 0, 1.0f, 1.0f, 1.0f, -1.0f, true }, kCurveHlg);
    s.inputStyle = kInputSmoothCanvas;
    s.detail = 2.0f;
    return s;
}
} // namespace detail

inline const Entry kLooks[] = {
    { Look::Native, "Native", "NVIDIA's own look, as games with built-in NR use it: Linear curve, no local tone.",
      false, detail::Native() },
    { Look::Natural, "Natural", "Natural style on the HLG curve. Passes 2 and 3 get their own tuned profiles.", false,
      detail::Natural() },
    { Look::Cinematic, "Cinematic", "The model's Cinematic style.", false, detail::Cinematic() },
    { Look::DetailOnly, "Detail only", "Adds detail and keeps the game's colours: Colour strength 0.", false,
      detail::DetailOnly() },
    { Look::Faces, "Faces", "Full effect on skin, less on scenery (environment detail 0.6).", false, detail::Faces() },
    { Look::Painterly, "Painterly", "Experimental: the picture is shown to the model as a watercolour.", true,
      detail::Painterly() },
    { Look::SoftCanvas, "Soft canvas",
      "Experimental: smooth canvas input on the HLG curve, Detail strength 2: smooth surfaces, strong detail.", true,
      detail::SoftCanvas() },
};

inline constexpr size_t kLookCount = sizeof(kLooks) / sizeof(kLooks[0]);

inline const Entry& Get(Look look) { return kLooks[(size_t) look]; }

inline bool Near(float a, float b) { return std::fabs(a - b) < 0.005f; }

inline bool Same(const Profile& a, const Profile& b)
{
    return a.style == b.style && Near(a.intensity, b.intensity) && Near(a.structure, b.structure) &&
           Near(a.tone, b.tone) && Near(a.skin, b.skin) && a.autoMask == b.autoMask;
}

inline bool Same(const std::optional<Profile>& a, const std::optional<Profile>& b)
{
    return a.has_value() == b.has_value() && (!a.has_value() || Same(*a, *b));
}

// Whether `now` is `look` in effect, for the button's highlight. Derived from the settings, never remembered.
inline bool Matches(const Settings& now, const Settings& look)
{
    return Same(now.pass1, look.pass1) && Same(now.pass2, look.pass2) && Same(now.pass3, look.pass3) &&
           now.laterOverrides == look.laterOverrides && now.curve == look.curve && Near(now.detail, look.detail) &&
           Near(now.colour, look.colour) && now.skinSplit == look.skinSplit && now.skinTone == look.skinTone &&
           Near(now.skinDetail, look.skinDetail) && Near(now.skinColour, look.skinColour) &&
           Near(now.envDetail, look.envDetail) && Near(now.envColour, look.envColour) &&
           now.inputStyle == look.inputStyle && Near(now.inputStrength, look.inputStrength) &&
           now.facesOnly == look.facesOnly;
}
} // namespace DlssNrLook
