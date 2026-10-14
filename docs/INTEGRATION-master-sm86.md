# OptiScaler master + F5 0.1.30 + built-in SM86

This integration is based on the fork's `main` commit
`37b8208f3338211be4a46ec9d010c533dd038325`, containing
release **0.1.30**, and OptiScaler `master` at
`1e036aef4cadc4388fd472dd0e74b5d98951709a`.

The fork's multipass Neural Rendering, Optical F5Low, native input adapters,
colour processing, depth guides, RTX 40 unlock, forwarder and page-based menu are
retained. Upstream changes include native Vulkan FG, XeFG inputs, new low latency
inputs/outputs, shader timings, resource tracking, per-feature DX11 caches and
per-game configuration defaults. Fork-specific REFramework, KCD2 HDR and MGSV
resize fixes are retained in the new game-defaults structure.

## RTX 20/30 MFG

The build embeds **SilyNoMeta/dlssg_for_sm86 0.3.5-5**, pinned in
[`redist/sm86/manifest.json`](../redist/sm86/manifest.json). The binary repository
does not provide its implementation source. The original binary is included as
a resource, rather than pretending it has been rebuilt as OptiScaler code.
`get_sm86_runtime.ps1` obtains and verifies the build input and its notices.
Every build with the runtime includes those licences in its output.

When enabled, OptiScaler extracts its embedded proxy automatically to
`OptiScaler/dlssg_sm86/version.dll` beside OptiScaler, verifies its SHA-256 and loads
it outside `DllMain`. The game's own `version.dll` is not replaced. No separate
plugin or ReShade panel installation is required.

Use **Frame Generation > RTX 20 / 30 MFG Unlocker**:

1. Enable built-in MFG, Save Settings and restart the game.
2. Enable native NVIDIA DLSS Frame Generation in the game.
3. Select **Follow game**, **X2–X6**, or **Dynamic / adaptive MFG** in OptiScaler.
4. Save Settings to keep your selection across restarts.

The factor and target are sent to `DLSSG_RequestControl`; state is queried through
`DLSSG_GetControlStatus`. The adapter is restricted to the exact pinned binary,
with its version-1 structure sizes checked by `tests/sm86_runtime_contract.py`.
The menu reports the runtime's observed factor separately from the requested one.
The initial saved request is retried until asynchronous runtime initialization
finishes, so opening the menu is not required for saved settings to apply.

The runtime handles native Streamline presentation; OptiScaler handles its menu
controls. OptiScaler's other FG engines are disabled for that session to avoid
two engines owning the same swapchain. The old SM86 setting no longer persists
`FrameGen/External=true`, and saves migrate that old automatic setting to false.
An existing unrelated external FG configuration remains usable with SM86 off.

Restart after changing the runtime's generated-frame ceiling or enabling/disabling
it. Multiplier and dynamic target changes apply through the live API. X6 requires
the game's plugin to support it. RTX 20 remains experimental. Native Vulkan MFG
still requires the game's matching NVIDIA **310.9.1** runtime, as documented by
SilyNoMeta; the integration does not overwrite game runtimes. Native Vulkan GPU
validation and compatibility with OptiScaler's own DLSSG output are not claimed.

## Build and validation

Use Visual Studio 2022 C++ tools, the Windows SDK and the repository's submodules:

```powershell
git submodule update --init --recursive
.\get_sm86_runtime.ps1
python tests/sm86_runtime_contract.py
msbuild OptiScaler.sln /m /p:Platform=x64 /p:Configuration=Release /p:BundleSm86Runtime=true
```

`BundleSm86Runtime` defaults to true. Set it to false for a source-only OptiScaler
build; the integrated option then reports a missing runtime unless the exact
pinned image is provided at its internal path. Packaging keeps the existing
`AcceptAmpereMfgLicenses` requirement for bundles containing the NVIDIA materials.
`package_release.ps1 -IncludeAmpereMfg -AcceptAmpereMfgLicenses` builds the embedded
variant and includes notices; it no longer installs a separate proxy in the ZIP.

The `integration_validation.yml` workflow builds Debug, Release and ReleaseDebug
on Windows and runs selected host tests. It has been prepared, **not executed**
in this Linux workspace. No compiled DLL or game/GPU validation is supplied here.

Local checks: 36 host tests passed, including control mapping, Vulkan depth
selection, menu navigation, native latency rules, colour/exposure and retry rules.
The pinned PE image/export ABI and all notice digests passed the offline contract
check. The fork's unmodified `nr_depth_finder_core_smoke` failed its concurrent
serial-log equality check; it remains a baseline limitation. Windows-dependent
and GPU tests could not be executed here. These checks do not establish that the
DLL compiles or behaves correctly in a game.

Before releasing, run the Windows workflow and test X2/X3/X4/X5/X6, follow-game and
dynamic mode on an RTX 30 game; then test NR on/off and multiple passes, saved
settings after restart, and enabling/disabling SM86 without a second FG provider.
