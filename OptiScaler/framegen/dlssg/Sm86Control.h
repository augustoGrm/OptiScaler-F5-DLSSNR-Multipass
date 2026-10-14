#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>

// Binary ABI of SilyNoMeta/dlssg_for_sm86 v0.3.5-5, version.dll SHA256
// 7489a89bf593cda243d95c62e6d8b75905d9774e27b7b2d53b91625a79c05ca4.
// This repository ships no SDK. Size/version checks and field offsets were checked
// against DLSSG_RequestControl and DLSSG_GetControlStatus in the pinned image.
// Never use these declarations with an unverified replacement binary.
namespace AmpereMfgLoader::Control
{
enum class Mode : uint32_t
{
    Saved = 0,
    FollowGame = 1,
    Fixed = 2,
    Dynamic = 3,
};

struct Request
{
    uint32_t size = 20;
    uint32_t version = 1;
    Mode mode = Mode::FollowGame;
    uint32_t multiplier = 0;
    uint32_t targetFps = 0;
};

struct RuntimeStatus
{
    uint32_t size = 92;
    uint32_t version = 1;
    uint32_t requestSerial = 0;
    uint32_t appliedSerial = 0;
    uint32_t requestedMode = 0;
    uint32_t requestedMultiplier = 0;
    uint32_t requestedTargetFps = 0;
    uint32_t savedMultiplier = 0;
    uint32_t savedDynamic = 0;
    uint32_t savedTargetFps = 0;
    uint32_t maxMultiplier = 0;
    uint32_t dynamicSupport = 0;
    uint32_t optionsSeen = 0;
    uint32_t observedMultiplier = 0;
    uint32_t dynamicActive = 0;
    uint32_t reserved[8] = {};
};

static_assert(sizeof(Request) == 20 && offsetof(Request, multiplier) == 12);
static_assert(sizeof(RuntimeStatus) == 92 && offsetof(RuntimeStatus, observedMultiplier) == 52);

inline Request MakeRequest(int multiplier, bool dynamic, int targetFps)
{
    Request request;
    request.mode = dynamic ? Mode::Dynamic : multiplier >= 2 ? Mode::Fixed : Mode::FollowGame;
    request.multiplier = request.mode == Mode::Fixed ? std::clamp(multiplier, 2, 6) : 0;
    request.targetFps = std::clamp(targetFps, 0, 1000);
    return request;
}
} // namespace AmpereMfgLoader::Control
