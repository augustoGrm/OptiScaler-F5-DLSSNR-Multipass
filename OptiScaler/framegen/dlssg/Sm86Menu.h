#pragma once

#include "AmpereMfgLoader.h"

// Kept beside the runtime adapter so the menu never has to duplicate its ABI.
template <class Help> void RenderSm86Menu(Config* config, Help help)
{
    if (!ImGui::CollapsingHeader("RTX 20 / 30 MFG Unlocker"))
        return;

    bool enabled = config->FGDLSSGAmpereMfgUnlock.value_or_default();
    ImGui::BeginDisabled(config->FGDLSSGAdaMfgUnlock.value_or_default());
    if (ImGui::Checkbox("Enable built-in MFG (restart)##sm86", &enabled))
    {
        config->FGDLSSGAmpereMfgUnlock = enabled;
        if (enabled)
        {
            config->FGDLSSGAdaMfgUnlock = false;
            config->ExternalFrameGeneration = false;
        }
    }
    ImGui::EndDisabled();
    help("Built into OptiScaler. Save Settings and restart to enable or disable.\n"
         "Requires a game with native NVIDIA DLSS Frame Generation.\n"
         "RTX 30 supported; RTX 20 experimental. Do not install another MFG unlocker alongside it.");

    if (!enabled)
        return;

    AmpereMfgLoader::UpdateControls();
    const auto status = AmpereMfgLoader::LastStatus();
    if (!status.ErrorMessage.empty())
        ImGui::TextWrapped("MFG: %s", status.ErrorMessage.c_str());
    else if (!status.DllLoaded)
        ImGui::TextWrapped("Save Settings and restart to load built-in MFG.");
    else if (!status.ControlReady)
        ImGui::TextWrapped("MFG runtime is initializing...");

    int multiplier = config->FGDLSSGAmpereMfgMultiplier.value_or_default();
    const char* factors[] = { "Follow game", "X2", "X3", "X4", "X5", "X6" };
    int choice = multiplier >= 2 ? std::clamp(multiplier - 1, 1, 5) : 0;
    bool dynamic = config->FGDLSSGAmpereMfgDynamic.value_or_default();
    bool changed = false;
    ImGui::BeginDisabled(dynamic);
    if (ImGui::Combo("FG multiplier##sm86", &choice, factors, 6))
    {
        config->FGDLSSGAmpereMfgMultiplier = choice == 0 ? 0 : choice + 1;
        changed = true;
    }
    ImGui::EndDisabled();
    help("Requests the actual multiplier from OptiScaler. X4 means one rendered frame plus three generated frames.\n"
         "Changes apply during this session; Save Settings keeps them.\n"
         "The game's Streamline plugin must support the requested factor.");

    if (ImGui::Checkbox("Dynamic / adaptive MFG##sm86", &dynamic))
    {
        config->FGDLSSGAmpereMfgDynamic = dynamic;
        changed = true;
    }
    if (dynamic)
    {
        int target = config->FGDLSSGAmpereMfgTargetFPS.value_or_default();
        if (ImGui::SliderInt("Target FPS##sm86", &target, 0, 1000, target == 0 ? "Display refresh" : "%d"))
        {
            config->FGDLSSGAmpereMfgTargetFPS = target;
            changed = true;
        }
        help("A target for NVIDIA's dynamic mode on DX12 or the runtime's adaptive mode on Vulkan, not an FPS cap.");
    }

    int ceiling = config->FGDLSSGAmpereMfgMaxFrames.value_or_default();
    if (ImGui::SliderInt("Maximum generated frames (restart)##sm86", &ceiling, 1, 5))
        config->FGDLSSGAmpereMfgMaxFrames = ceiling;
    help("Runtime capability ceiling: five generated frames allows up to X6. Save Settings and restart after changing "
         "it.");

    if (changed)
        AmpereMfgLoader::UpdateControls();
    if (status.Runtime)
    {
        ImGui::Text("Runtime limit: X%u", status.Runtime->maxMultiplier);
        if (status.Runtime->observedMultiplier)
            ImGui::Text("Runtime reports: X%u", status.Runtime->observedMultiplier);
        if (!status.ControlAccepted)
            ImGui::TextWrapped("The runtime has not accepted the requested settings.");
    }
}
