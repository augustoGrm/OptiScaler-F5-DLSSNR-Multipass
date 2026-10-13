#include "pch.h"

#include "input_uell.h"
#include "UeInputRule.h"

#include <hooks/Reflex_Hooks.h>
#include <native/NativeLowLatency.h>

#include <DirectXMath.h>

using namespace DirectX;

// The output keeps what it has while the mod stands aside: stopping Reflex would send low latency off under the one
// that runs
static bool StandAside()
{
    const bool aside = ueinput::StandAside(ReflexHooks::gameCalledReflex(), native::lowlatency::Running());

    static std::atomic_bool wasAside = false;

    if (wasAside.exchange(aside) != aside)
    {
        if (aside)
            LOG_INFO("UE Low Latency mod: stands aside, the game's Reflex or OptiScaler's own low latency runs");
        else
            LOG_INFO("UE Low Latency mod: its frames drive the low latency output again");
    }

    return aside;
}

void InputUeLowLatency::init()
{
    if (inited)
        return;

    // The swapchain's device, D3D11 games can have a D3D12 device too
    if (State::Instance().swapchainApi == DX11)
        device = State::Instance().currentD3D11Device;
    else
        device = State::Instance().currentD3D12Device;

    inited = device != nullptr;

    if (inited)
        LOG_DEBUG("Inited UeLowLatency");
}

void InputUeLowLatency::tickStart(int64_t frameId, float DeltaSeconds, bool bIdleMode)
{
    if (!inited)
        InputUeLowLatency::init();

    if (!inited || StandAside())
        return;

    if (auto lowLatency = ueinput::SleepModeToSend(native::lowlatency::GameLowLatencyBlocked(), sentLowLatency))
    {
        // No interval: the frame cap stays with OptiScaler's limiter. InputCommon keeps the mode even when it fails,
        // for an output that starts later.
        SleepMode sleepMode {};
        sleepMode.low_latency_enabled = *lowLatency;
        sentLowLatency = *lowLatency;

        if (auto result = InputCommon::set_sleep_mode(inputContext, device, &sleepMode);
            result != InputResult::Ok && result != InputResult::UsingDifferentInput)
        {
            LOG_ERROR("set_sleep_mode result: {}", magic_enum::enum_name(result));
        }
    }

    // Most backends use 32bit frameId so it is what it is
    auto result = InputCommon::sleep(inputContext, device, (uint32_t) frameId);

    if (result == InputResult::UsingDifferentInput)
        return;

    if (result != InputResult::Ok)
    {
        LOG_ERROR("sleep result: {}", magic_enum::enum_name(result));
        return;
    }

    MarkerParams markerParams {};
    markerParams.frame_id = frameId;
    markerParams.marker_type = MarkerType::SIMULATION_START;

    result = InputCommon::set_marker(inputContext, device, markerParams);

    if (result != InputResult::Ok)
    {
        LOG_ERROR("set_marker result: {}", magic_enum::enum_name(result));
        return;
    }
}

void InputUeLowLatency::tickEnd(int64_t frameId, float DeltaSeconds, bool bIdleMode)
{
    if (!inited || StandAside())
        return;

    MarkerParams markerParams {};
    markerParams.frame_id = frameId;
    markerParams.marker_type = MarkerType::SIMULATION_END;

    auto result = InputCommon::set_marker(inputContext, device, markerParams);

    if (result != InputResult::Ok)
    {
        LOG_ERROR("set_marker result: {}", magic_enum::enum_name(result));
        return;
    }
}

static void GetCameraBasis(const float cameraRotation[3], float cameraUp[3], float cameraRight[3],
                           float cameraForward[3])
{
    float pitch = DirectX::XMConvertToRadians(cameraRotation[0]);
    float yaw = DirectX::XMConvertToRadians(cameraRotation[1]);
    float roll = DirectX::XMConvertToRadians(cameraRotation[2]);

    float SP, CP, SY, CY, SR, CR;
    DirectX::XMScalarSinCos(&SP, &CP, pitch);
    DirectX::XMScalarSinCos(&SY, &CY, yaw);
    DirectX::XMScalarSinCos(&SR, &CR, roll);

    cameraForward[0] = CP * CY;
    cameraForward[1] = CP * SY;
    cameraForward[2] = SP;

    cameraRight[0] = SR * SP * CY - CR * SY;
    cameraRight[1] = SR * SP * SY + CR * CY;
    cameraRight[2] = -SR * CP;

    cameraUp[0] = -(CR * SP * CY + SR * SY);
    cameraUp[1] = CY * SR - CR * SP * SY;
    cameraUp[2] = CR * CP;
}

void InputUeLowLatency::cameraUpdate(int64_t frameId, float cameraPosition[3], float cameraRotation[3], float fovAngle)
{
    // Technically FSRFG input might have the camera data but not guaranteed
    // if (State::Instance().activeFgInput == FGInput::DLSSG)
    //    return;

    if (auto fg = State::Instance().currentFG)
    {
        auto index = (int) (frameId % BUFFER_COUNT);

        float cameraUp[3];
        float cameraRight[3];
        float cameraForward[3];

        GetCameraBasis(cameraRotation, cameraUp, cameraRight, cameraForward);

        fg->SetCameraData(cameraPosition, cameraUp, cameraRight, cameraForward, index);

        UINT64 width = 0;
        UINT height = 0;
        fg->GetInterpolationRect(width, height, index);
        float aspectRatio = (float) width / (float) height;

        // TODO: camera near and far are placeholders
        fg->SetCameraValues(0.01f, 10000.f, DirectX::XMConvertToRadians(fovAngle), aspectRatio, 0.0f, index);
    }
}
