@echo off
setlocal
set "OPTISCALER_SCRIPT=%~f0"
cd /d "%~dp0"
powershell -NoProfile -ExecutionPolicy Bypass -Command "$text=[IO.File]::ReadAllText($env:OPTISCALER_SCRIPT); $marker=[char]35+' OPTISCALER_POWERSHELL_START'; & ([scriptblock]::Create($text.Substring($text.LastIndexOf($marker)+$marker.Length))) -ScriptPath $env:OPTISCALER_SCRIPT"
set "optiExit=%errorlevel%"
if "%optiExit%"=="10" (
    del /q "%~f0"
    exit /b 0
)
if not defined CI pause
exit /b %optiExit%
# OPTISCALER_POWERSHELL_START
param([Parameter(Mandatory=$true)][string]$ScriptPath)
$ErrorActionPreference = 'Stop'

$directory = Split-Path -Parent $ScriptPath
$proxyNames = @('dxgi.dll', 'winmm.dll', 'version.dll', 'dbghelp.dll', 'd3d12.dll', 'wininet.dll', 'winhttp.dll', 'OptiScaler.asi')

function Test-OptiScaler([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { return $false }
    return (Get-Item -LiteralPath $Path).VersionInfo.OriginalFilename -ieq 'OptiScaler.dll'
}

function Get-InstalledDlls {
    @('OptiScaler.dll') + $proxyNames | ForEach-Object {
        $path = Join-Path $directory $_
        if (Test-OptiScaler $path) { $path }
    }
}

function Read-ProxyName {
    while ($true) {
        Write-Host ''
        Write-Host 'Choose an OptiScaler filename (winmm.dll is recommended for Vulkan):'
        for ($i = 0; $i -lt $proxyNames.Count; $i++) {
            Write-Host ('  [{0}] {1}' -f ($i + 1), $proxyNames[$i])
        }
        Write-Host '  [0] Cancel'
        $answer = Read-Host 'Choice (Enter = dxgi.dll)'
        if ($answer -eq '0') { return $null }
        if ($answer -eq '') { $answer = '1' }
        $index = 0
        if ([int]::TryParse($answer, [ref]$index) -and $index -ge 1 -and $index -le $proxyNames.Count) {
            return $proxyNames[$index - 1]
        }
        Write-Host 'Invalid choice.'
    }
}

function Rename-InstalledDll([string]$Source) {
    while ($true) {
        $name = Read-ProxyName
        if (-not $name) { return $false }
        $target = Join-Path $directory $name
        if ($target -ieq $Source) {
            Write-Host 'OptiScaler already uses that filename.'
            return $true
        }
        if (Test-Path -LiteralPath $target) {
            Write-Host "$name already exists. It will not be overwritten. Choose another filename."
            continue
        }
        Move-Item -LiteralPath $Source -Destination $target -ErrorAction Stop
        Write-Host "OptiScaler now uses $name. Settings and dependencies were kept."
        return $true
    }
}

try {
    if ((Test-Path -LiteralPath (Join-Path $directory 'OptiScaler.sln')) -or
        (Test-Path -LiteralPath (Join-Path $directory '.git'))) {
        throw 'This is a source checkout. Extract the compiled Windows package into the game executable folder instead.'
    }

    $maintenance = [IO.Path]::GetFileName($ScriptPath) -ieq 'Remove_OptiScaler.bat'
    if ($maintenance) {
        Write-Host 'OptiScaler - remove or rename'
        $installed = @(Get-InstalledDlls)
        if ($installed.Count -eq 0) {
            Write-Host 'No DLL identified as OptiScaler was found. Other mods were left untouched.'
            exit 0
        }
        $selected = $installed[0]
        Write-Host ''
        Write-Host 'Verified OptiScaler DLLs:'
        for ($i = 0; $i -lt $installed.Count; $i++) {
            Write-Host ('  [{0}] {1}' -f ($i + 1), [IO.Path]::GetFileName($installed[$i]))
        }
        if ($installed.Count -gt 1) {
            $answer = Read-Host 'Select a DLL (0 = cancel)'
            $index = 0
            if (-not [int]::TryParse($answer, [ref]$index) -or $index -lt 1 -or $index -gt $installed.Count) { exit 0 }
            $selected = $installed[$index - 1]
        }
        Write-Host ''
        Write-Host '  [1] Delete this OptiScaler DLL (keep settings and dependencies)'
        Write-Host '  [2] Rename this OptiScaler DLL'
        Write-Host '  [0] Cancel'
        $action = Read-Host 'Choice'
        if ($action -eq '2') {
            [void](Rename-InstalledDll $selected)
            exit 0
        }
        if ($action -ne '1') { exit 0 }
        # Recheck the version resource immediately before removal; filenames alone never establish ownership.
        if (-not (Test-OptiScaler $selected)) { throw 'The selected file is no longer an OptiScaler DLL.' }
        Remove-Item -LiteralPath $selected -ErrorAction Stop
        Write-Host 'OptiScaler DLL removed. Your configuration, dependencies and other mods were kept.'
        if (@(Get-InstalledDlls).Count -eq 0) { exit 10 }
        exit 0
    }

    Write-Host 'OptiScaler - Windows setup'
    $source = Join-Path $directory 'OptiScaler.dll'
    if (-not (Test-OptiScaler $source)) {
        throw 'OptiScaler.dll is missing or is not a verified OptiScaler build.'
    }
    if (Test-Path -LiteralPath (Join-Path $directory 'Engine')) {
        Write-Host 'Unreal Engine games usually need installation in <game>\Binaries\Win64, beside the game executable.'
        if ((Read-Host 'Continue in this folder? (y/N)') -ine 'y') { exit 0 }
    }
    $otherInstalls = @(Get-InstalledDlls | Where-Object { $_ -ine $source })
    if ($otherInstalls.Count -gt 0) {
        Write-Host 'An installed OptiScaler DLL already exists:'
        $otherInstalls | ForEach-Object { Write-Host ('  ' + [IO.Path]::GetFileName($_)) }
        throw 'Use Remove_OptiScaler.bat to remove the previous DLL, then run setup again. No files were changed.'
    }
    while ($true) {
        $name = Read-ProxyName
        if (-not $name) { exit 0 }
        $target = Join-Path $directory $name
        if (-not (Test-Path -LiteralPath $target)) { break }
        Write-Host "$name already exists. Setup will not overwrite another mod. Choose a different filename."
    }
    if ($name -ieq 'd3d12.dll') {
        Write-Host 'd3d12.dll can conflict with Streamline in some games. Use dxgi.dll or winmm.dll if DLSS options disappear.'
    }
    $remove = Join-Path $directory 'Remove_OptiScaler.bat'
    $oldRemove = $null
    if (Test-Path -LiteralPath $remove) {
        # Keep a preexisting maintenance script if installation fails.
        $oldRemove = [IO.File]::ReadAllBytes($remove)
    }
    try {
        Copy-Item -LiteralPath $ScriptPath -Destination $remove -Force -ErrorAction Stop
        Move-Item -LiteralPath $source -Destination $target -ErrorAction Stop
    } catch {
        if ($null -ne $oldRemove) { [IO.File]::WriteAllBytes($remove, $oldRemove) }
        elseif (Test-Path -LiteralPath $remove) { Remove-Item -LiteralPath $remove }
        throw
    }
    Write-Host ''
    Write-Host "Installed as $name. SM86 is built in and configured automatically for RTX 20/30."
    Write-Host 'Press Insert in the game to open OptiScaler. Save Settings and Close are at the bottom.'
    Write-Host 'Use Remove_OptiScaler.bat to change the proxy filename or remove the installed DLL.'
    Write-Host 'Setup is complete; setup_windows.bat will now remove itself.'
    exit 10
} catch {
    Write-Host ('ERROR: ' + $_.Exception.Message) -ForegroundColor Red
    exit 1
}
