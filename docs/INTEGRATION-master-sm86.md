# OptiScaler master + F5 0.1.30 + native SM86 backend

This branch integrates the F5 fork's `main` at
`37b8208f3338211be4a46ec9d010c533dd038325` (release **0.1.30**) with OptiScaler
`master` at `1e036aef4cadc4388fd472dd0e74b5d98951709a`. Multipass Neural Rendering,
Optical F5Low, native input adapters, colour processing and the page menu remain
available alongside the imported upstream features.

## Frame Generation ownership

SM86 is selected automatically on eligible NVIDIA Turing/Ampere hardware. There
is no RTX 20/30 unlocker toggle, companion panel or separate unlocker installation.
RTX 20 support remains experimental. Other GPUs do not load this backend.

Enabling the backend does not force Frame Generation on. Select the normal
**FG Input**, **FG Output: DLSSG** and **Frame Gen** mode in OptiScaler. Its usual
Off, fixed multiplier, follow-game and dynamic controls remain authoritative.
Follow-game is available when the selected input can report the game's setting;
dynamic controls appear when the active plugin reports support. **Save Settings**
persists these settings, and **Close** hides the overlay. Changing FG input or
output retains the normal restart requirements.

| Component | Responsibility |
| --- | --- |
| OptiScaler input adapters | Collect depth, motion vectors, HUDless colour and frame constants from the selected input. |
| OptiScaler `DLSSG_Dx12` and its Streamline instance | Own the FG swapchain, resource tags, presentation, enable/disable state and `slDLSSGSetOptions` calls. |
| Embedded SM86 backend | Install the NGX runtime compatibility and compute changes beneath that same pipeline. |
| Active Streamline plugin | Report the available generated-frame count and dynamic-mode support. |

The integration uses the native backend and NVIDIA runtime contained in
**SilyNoMeta/dlssg_for_sm86 0.3.5-5**, pinned by
[`redist/sm86/manifest.json`](../redist/sm86/manifest.json). The build verifies the
downloaded provenance image, extracts its nested resources, and embeds only:

- NVIDIA `nvngx_dlssg.dll` **310.9.1**, SHA-256
  `ff6e90eb78b827927dff5b4ecc6b1c870c2e9bca29ed9f48c7d348cc9e170b82`.
- `sm86_backend.dll`, SHA-256
  `6cc675209e55bb3ddb6b1ba8008eef93f633d3e2207c3a073df321a6a8ed4e59`.

Neither the outer universal proxy nor its inner loader proxy is executed.
OptiScaler calls the backend's pinned, versioned `DlssgMod_*` ABI directly. It
prepares the backend outside `DllMain`, before loading its own Streamline stack,
then binds that stack's DLSSG plugin. Architecture checks are scoped to the local
plugin; the game's DLSS/DLSSD architecture identity is preserved.

The backend's internal ceiling is five generated frames, allowing up to **X6**.
The menu exposes the active plugin's reported capabilities; the ceiling alone
does not establish that a particular multiplier works in a game. Temporary
capability limits do not overwrite the saved multiplier. The obsolete
`AmpereMfgMultiplier`, `AmpereMfgDynamic` and companion INI controls are not used.
The old automatic `External=true` handoff is migrated, while an explicit external
provider can still use OptiScaler's separate external-FG setting.

Windows loads the verified embedded components from
`OptiScaler/dlssg_sm86/` beside the installed OptiScaler DLL. Extraction is
automatic and preserves their original bytes. No game proxy or game runtime is
overwritten. A separately installed SM86 proxy must be removed before using the
built-in backend, to avoid a second owner patching the same runtime.

The source repository supplies binaries rather than the implementation source of
those SM86 components. This integration does not claim to rebuild their kernels
or include the outer proxy's companion controls and image-quality features.
OptiScaler's DLSSG output remains subject to its existing API support; this change
does not add a native Vulkan DLSSG output.

## Embedded Neural Rendering pair

The two DLLs supplied for this build are preserved byte for byte and embedded in
`OptiScaler.dll`:

| Original filename | Bytes | SHA-256 |
| --- | ---: | --- |
| `nvngx.dll_dlssnr.dll` | 152064 | `89766bcb38a9caea9a2e3f767923e6cda8b9b51a6a66ca14ac9473d734e12efa` |
| `nvngx_dlssnr.dll` | 165840496 | `e67dee209320cdafe0e93e45675d7aa34323a53acc57a72b2e40a181581c989a` |

[`redist/nr/manifest.json`](../redist/nr/manifest.json) records their identity.
The large model is stored as six verified build-input parts;
`get_nr_runtime.ps1` reconstructs the original DLL before resource compilation.
No replacement model or external download is needed.

At runtime OptiScaler installs the pair in an internal, hash-specific directory
under `OptiScaler/dlssnr/`. The forwarder keeps its original filename because the
model checks its caller. The model directory is included before NGX's first SR
or FG initialization, which can cache the feature search list. No separate NR
DLL installation is required for the compiled Windows package.

## Windows package and maintenance

Extract the entire compiled package beside the game's rendering executable and
run `setup_windows.bat`. Choose an available proxy filename; setup checks existing
files and will not overwrite another mod. It creates `Remove_OptiScaler.bat`, then
deletes its own setup script after successful installation.

`Remove_OptiScaler.bat` can rename the installed OptiScaler DLL or remove it. It
checks the DLL's version resource rather than trusting its filename, preserves
configuration and dependencies, and removes its maintenance script after the
last identified OptiScaler DLL is removed. Close the game before maintenance.

The package is assembled from an explicit allowlist. It contains operational
Windows dependencies and the required Streamline plugins, with NR and SM86 inside
the main DLL. Build imports, PDBs, logs, standalone NR/SM86 proxies, Linux setup
files and loose notice folders are excluded. Third-party notices are embedded and
available through the overlay's **Licenses** button. Windows packaging does not
remove the repository's Linux source compatibility or Linux setup sources.

## Build and validation

Use Visual Studio 2022 C++ tools, the Windows SDK and the repository's submodules:

```powershell
git submodule update --init --recursive
.\get_streamline.ps1 -Destination .dependencies/streamline/verified/streamline -AcceptNvidiaLicenses
.\get_sm86_runtime.ps1
.\get_nr_runtime.ps1
python tests/sm86_runtime_contract.py
python tests/nr_runtime_contract.py
msbuild OptiScaler.sln /m /p:Platform=x64 /p:Configuration=Release /p:BundleSm86Runtime=true /p:BundleNrRuntime=true
```

Both embedding properties default to true. The compiled Windows package requires
both payload sets and checks the DLL resources before assembly. Source builds
that disable embedding retain their separate runtime requirements.

To build and package with the pinned dependencies and embedded notices:

```powershell
.\package_release.ps1 -Version v0.1.30-master-sm86 -AcceptNvidiaLicenses -AcceptAmpereMfgLicenses
```

The integration workflow builds Debug, Release and ReleaseDebug, runs the binary
contract and selected host tests, checks Windows installation/maintenance, and
uploads the assembled Release ZIP. Check the workflow for the exact commit being
downloaded: successful results from the earlier proxy-based integration do not
validate this backend change.

The new Windows compilation and package checks are pending until that workflow
completes. Offline hashes, PE/ABI checks and host tests cannot establish in-game
operation, performance or image quality. GPU validation still requires an RTX 30
machine: test fixed multipliers, follow-game, dynamic mode, FG off/on, NR off/on
and multipass, saved settings after restart, and operation without an external FG
provider. RTX 20 and each graphics API require their own compatibility testing.
