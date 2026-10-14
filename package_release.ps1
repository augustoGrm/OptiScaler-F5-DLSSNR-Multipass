# Assemble the Windows package from an explicit runtime allowlist.
# NR, SM86 and third-party notices are resources inside OptiScaler.dll.
param(
    [ValidatePattern('^[A-Za-z0-9][A-Za-z0-9._-]*$')]
    [string]$Version = 'v0.1.30-master-sm86',
    [switch]$SkipBuild,
    [switch]$PackageOutputsOnly,
    [switch]$IncludeDlssFrameGeneration = $true,
    [switch]$AcceptNvidiaLicenses,
    [switch]$IncludeAmpereMfg = $true,
    [switch]$AcceptAmpereMfgLicenses,
    [string]$HybridAssetsDirectory,
    [string]$StreamlineArchive,
    [string]$PortBackendDll,
    [string]$PortBackendIni
)
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot

if ($PSBoundParameters.ContainsKey('Version') -and $Version -match '(\d+\.\d+\.\d+)') {
    $tagged = $Matches[1]
    $header = Get-Content -LiteralPath "$root\OptiScaler\resource.h" -Raw
    $baked = foreach ($part in 'MAJOR', 'MINOR', 'HOTFIX') {
        if ($header -match "#define NR_RELEASE_${part}_VERSION\s+(\d+)") { $Matches[1] }
        else { throw "NR_RELEASE_${part}_VERSION not found in OptiScaler\resource.h" }
    }
    if (($baked -join '.') -ne $tagged) { throw "Version $Version differs from the DLL version ($($baked -join '.'))." }
}
if ($PortBackendDll -or $PortBackendIni) {
    throw 'Standalone NR backends are not part of the integrated Windows package. Supply the pinned embedded NR inputs instead.'
}
if (-not $IncludeAmpereMfg) { throw 'This Windows build requires the integrated SM86 backend.' }
if (-not $IncludeDlssFrameGeneration) { throw 'Native DLSS FG requires the bundled Streamline stack.' }
if (-not $AcceptAmpereMfgLicenses) { throw 'Read the embedded SM86 notices and pass -AcceptAmpereMfgLicenses to package this build.' }
if ($IncludeDlssFrameGeneration -and -not $AcceptNvidiaLicenses) { throw 'Bundling the native Streamline stack requires -AcceptNvidiaLicenses.' }

$flavour = '-builtin-nr-sm86'
$stage = "$root\release\$Version$flavour"
$zip = "$root\release\OptiScaler-DLSSNR-F5-$Version$flavour.zip"
if ((Test-Path -LiteralPath $stage) -or (Test-Path -LiteralPath $zip)) {
    throw 'Release output already exists. Choose a new -Version; existing packages are never overwritten.'
}

$streamlineSource = "$root\.dependencies\streamline\verified\streamline"
if (-not ($SkipBuild -or $PackageOutputsOnly)) {
    & "$root\get_streamline.ps1" -Destination $streamlineSource -ArchivePath $StreamlineArchive -AcceptNvidiaLicenses
    & "$root\get_sm86_runtime.ps1"
    & "$root\get_nr_runtime.ps1"
    $msb = (Get-Command MSBuild.exe -ErrorAction SilentlyContinue).Source
    if (-not $msb) {
        $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
        if (Test-Path -LiteralPath $vswhere) {
            $msb = & $vswhere -latest -products '*' -requires Microsoft.Component.MSBuild -find 'MSBuild\**\Bin\MSBuild.exe' | Select-Object -First 1
        }
    }
    if (-not $msb) { throw 'MSBuild.exe was not found. Install Visual Studio C++ build tools or use -PackageOutputsOnly with a verified Release build.' }
    & $msb "$root\OptiScaler.sln" /p:Configuration=Release /p:Platform=x64 /p:BundleSm86Runtime=true /p:BundleNrRuntime=true /v:minimal /m
    if ($LASTEXITCODE -ne 0) { throw "MSBuild failed with exit code $LASTEXITCODE." }
}

$src = "$root\x64\Release\a"
$builtDll = "$src\OptiScaler.dll"
if (-not (Test-Path -LiteralPath $builtDll -PathType Leaf)) { throw "Required build output is missing: $builtDll" }

# Inspect resources without running DllMain or any DLL code. A cleaned ZIP must never accidentally
# contain a DLL built without the NR payloads, SM86 backend, or accessible licence text.
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class OptiScalerPackageResources {
    [DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)]
    public static extern IntPtr LoadLibraryEx(string file, IntPtr handle, uint flags);
    [DllImport("kernel32.dll", EntryPoint="FindResourceW", SetLastError=true)]
    public static extern IntPtr FindResource(IntPtr module, IntPtr name, IntPtr type);
    [DllImport("kernel32.dll", SetLastError=true)]
    public static extern uint SizeofResource(IntPtr module, IntPtr resource);
    [DllImport("kernel32.dll")]
    public static extern bool FreeLibrary(IntPtr module);
}
'@
$module = [OptiScalerPackageResources]::LoadLibraryEx($builtDll, [IntPtr]::Zero, 0x22)
if ($module -eq [IntPtr]::Zero) { throw 'Could not inspect the packaged DLL resources.' }
try {
    foreach ($id in 31011, 31012, 31101, 31102, 31110) {
        $resource = [OptiScalerPackageResources]::FindResource($module, [IntPtr]$id, [IntPtr]10)
        if ($resource -eq [IntPtr]::Zero -or [OptiScalerPackageResources]::SizeofResource($module, $resource) -eq 0) {
            throw "OptiScaler.dll is missing required embedded resource $id. Rebuild before packaging."
        }
    }
} finally { [void][OptiScalerPackageResources]::FreeLibrary($module) }

New-Item -ItemType Directory -Force -Path $stage | Out-Null
Copy-Item -LiteralPath $builtDll -Destination "$stage\OptiScaler.dll"
foreach ($name in 'OptiScaler.ini', 'setup_windows.bat') {
    Copy-Item -LiteralPath "$root\$name" -Destination "$stage\$name"
}

# Only operational runtime DLLs are shipped beside the main DLL. Notice text is embedded and can
# be opened from the overlay. No build imports, logs, Linux setup, NR sidecars, or stale plugins.
$dependencyNames = @(
    'libxess.dll', 'libxess_dx11.dll', 'libxess_fg.dll', 'libxell.dll',
    'amd_fidelityfx_dx12.dll', 'amd_fidelityfx_vk.dll',
    'amd_fidelityfx_loader_dx12.dll', 'amd_fidelityfx_upscaler_dx12.dll',
    'amd_fidelityfx_framegeneration_dx12.dll', 'amd_fidelityfx_denoiser_dx12.dll',
    'amd_fidelityfx_radiancecache_dx12.dll'
)
$dependencyRoot = "$src\OptiScaler"
foreach ($name in $dependencyNames) {
    $source = Join-Path $dependencyRoot $name
    if (Test-Path -LiteralPath $source -PathType Leaf) {
        New-Item -ItemType Directory -Force -Path "$stage\OptiScaler" | Out-Null
        Copy-Item -LiteralPath $source -Destination "$stage\OptiScaler\$name"
    }
}
foreach ($required in 'libxess.dll', 'amd_fidelityfx_vk.dll', 'amd_fidelityfx_loader_dx12.dll', 'amd_fidelityfx_upscaler_dx12.dll', 'amd_fidelityfx_framegeneration_dx12.dll') {
    if (-not (Test-Path -LiteralPath "$stage\OptiScaler\$required" -PathType Leaf)) { throw "Required runtime dependency is missing: $required" }
}
$agility = "$src\OptiScaler\D3D12_OptiScaler\D3D12Core.dll"
if (-not (Test-Path -LiteralPath $agility -PathType Leaf)) { throw 'Required Agility SDK D3D12Core.dll is missing.' }
New-Item -ItemType Directory -Force -Path "$stage\OptiScaler\D3D12_OptiScaler" | Out-Null
Copy-Item -LiteralPath $agility -Destination "$stage\OptiScaler\D3D12_OptiScaler\D3D12Core.dll"

# LUTs are optional colour-processing data; preserve their function without packaging README files.
if (Test-Path -LiteralPath "$root\LUTs") {
    New-Item -ItemType Directory -Force -Path "$stage\OptiScaler\LUTs" | Out-Null
    Get-ChildItem -LiteralPath "$root\LUTs" -Filter '*.cube' -File | Copy-Item -Destination "$stage\OptiScaler\LUTs"
}
# Streamline is prepared before compilation so its notice text is part of resource 31110.
# Its NVIDIA runtime is already resource 31011; only the five SL plugins need to remain separate.
$streamlineManifest = Get-Content -LiteralPath "$root\redist\streamline\manifest.json" -Raw | ConvertFrom-Json
New-Item -ItemType Directory -Force -Path "$stage\OptiScaler\streamline" | Out-Null
foreach ($record in $streamlineManifest.files) {
    $source = Join-Path $streamlineSource $record.name
    if (-not (Test-Path -LiteralPath $source -PathType Leaf) -or
        (Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash -ne $record.sha256) {
        throw "Streamline input missing or changed: $($record.name). Run get_streamline.ps1 before building."
    }
    if ($record.name -like 'sl.*.dll') {
        Copy-Item -LiteralPath $source -Destination "$stage\OptiScaler\streamline"
    }
}
if ($HybridAssetsDirectory) {
    $manifest = Get-Content -LiteralPath (Join-Path $HybridAssetsDirectory 'asset-manifest.json') -Raw | ConvertFrom-Json
    foreach ($item in $manifest.files) {
        $assetRoot = [IO.Path]::GetFullPath((Join-Path $HybridAssetsDirectory 'OptiScaler/nvfp4/hybrid'))
        $source = [IO.Path]::GetFullPath((Join-Path $assetRoot $item.path))
        if (-not $source.StartsWith($assetRoot + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) { throw 'Invalid hybrid asset path.' }
        if ((Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash -ne $item.sha256) { throw "Hybrid asset hash mismatch: $source" }
        $target = Join-Path "$stage/OptiScaler/nvfp4/hybrid" $item.path
        New-Item -ItemType Directory -Force -Path (Split-Path -Parent $target) | Out-Null
        Copy-Item -LiteralPath $source -Destination $target
    }
}

$ini = Get-Content -LiteralPath "$stage\OptiScaler.ini" -Raw
if ($ini -notmatch '(?mi)^TargetProcessName=auto\s*$') { throw 'The packaged INI must not contain a game-specific process filter.' }
foreach ($key in 'FinishedPicture', 'DeferredDLSS', 'ResidualFG', 'ResidualFGApproxCamera', 'UnlockPasses', 'AdaMfgUnlock') {
    if ($ini -match "(?mi)^$key=true\s*$") { throw "Experimental option $key must not be enabled in the Windows package." }
}

# Verify normal Release security settings and reject an outer CodeView/PDB record. Embedded
# immutable vendor DLLs have their own records, which are unrelated to our build environment.
$peBytes = [IO.File]::ReadAllBytes("$stage\OptiScaler.dll")
$peHeader = [BitConverter]::ToUInt32($peBytes, 0x3c)
$dllCharacteristics = [BitConverter]::ToUInt16($peBytes, $peHeader + 94)
foreach ($flag in @{ 'HIGH_ENTROPY_VA' = 0x0020; 'DYNAMICBASE' = 0x0040; 'NX_COMPAT' = 0x0100 }.GetEnumerator()) {
    if (($dllCharacteristics -band $flag.Value) -eq 0) { throw "OptiScaler.dll is missing $($flag.Key)." }
}
$optionalHeader = $peHeader + 24
$optionalSize = [BitConverter]::ToUInt16($peBytes, $peHeader + 20)
$sectionCount = [BitConverter]::ToUInt16($peBytes, $peHeader + 6)
$magic = [BitConverter]::ToUInt16($peBytes, $optionalHeader)
$dataDirectories = $optionalHeader + $(if ($magic -eq 0x20b) { 112 } else { 96 })
$debugRva = [BitConverter]::ToUInt32($peBytes, $dataDirectories + 48)
$debugSize = [BitConverter]::ToUInt32($peBytes, $dataDirectories + 52)
if ($debugRva -ne 0 -and $debugSize -ne 0) {
    $debugOffset = $null
    for ($i = 0; $i -lt $sectionCount; $i++) {
        $section = $optionalHeader + $optionalSize + 40 * $i
        $virtualSize = [BitConverter]::ToUInt32($peBytes, $section + 8)
        $virtualAddress = [BitConverter]::ToUInt32($peBytes, $section + 12)
        $rawSize = [BitConverter]::ToUInt32($peBytes, $section + 16)
        $rawAddress = [BitConverter]::ToUInt32($peBytes, $section + 20)
        if ($debugRva -ge $virtualAddress -and $debugRva -lt ($virtualAddress + [Math]::Max($virtualSize, $rawSize))) {
            $debugOffset = $rawAddress + $debugRva - $virtualAddress
            break
        }
    }
    if ($null -eq $debugOffset) { throw 'Invalid PE debug directory.' }
    for ($i = 0; $i + 28 -le $debugSize; $i += 28) {
        if ([BitConverter]::ToUInt32($peBytes, $debugOffset + $i + 12) -eq 2) { throw 'OptiScaler.dll has a CodeView/PDB record. Use a Release build.' }
    }
}
$versionInfo = (Get-Item -LiteralPath "$stage\OptiScaler.dll").VersionInfo
foreach ($field in 'CompanyName', 'FileDescription', 'ProductName', 'OriginalFilename', 'LegalCopyright', 'InternalName') {
    if ([string]::IsNullOrWhiteSpace($versionInfo.$field)) { throw "OptiScaler.dll has an empty $field in its version resource." }
}
if ($versionInfo.OriginalFilename -ine 'OptiScaler.dll') { throw 'The DLL cannot be identified safely by the Windows installer.' }

$forbidden = Get-ChildItem -LiteralPath $stage -Recurse -File | Where-Object {
    $_.Name -match '\.(exp|lib|pdb|ilk|log|sh)$|^nvngx[._].*dlssnr.*|^nr_.*|^setup_linux|EXTRACT|LICENSE|NOTICE' -or
    $_.DirectoryName -match '[\\/]Licenses([\\/]|$)'
}
if ($forbidden) { throw "Unexpected files in Windows package: $($forbidden.FullName -join ', ')" }
$checksumLines = Get-ChildItem -LiteralPath $stage -Recurse -File | Sort-Object FullName | ForEach-Object {
    $relative = $_.FullName.Substring($stage.TrimEnd('\', '/').Length).TrimStart('\', '/').Replace('\', '/')
    '{0} *{1}' -f (Get-FileHash -Algorithm SHA256 -LiteralPath $_.FullName).Hash, $relative
}
[IO.File]::WriteAllLines("$stage\SHA256SUMS.txt", $checksumLines, [Text.UTF8Encoding]::new($false))
Compress-Archive -Path "$stage\*" -DestinationPath $zip -CompressionLevel Optimal
Write-Host "Windows ZIP: $zip"
Write-Host "Embedded NR, SM86 and licences verified; $($checksumLines.Count) runtime and installation files packaged."
