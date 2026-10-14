#pragma once

#include <filesystem>
#include <optional>

namespace DlssNrBundle
{
// Installs the exact embedded forwarder/model pair before the first NGX init.
// Builds without the resources keep the existing external-runtime search.
bool Ensure();
std::optional<std::filesystem::path> GetForwarder();
std::optional<std::filesystem::path> GetModel();
} // namespace DlssNrBundle
