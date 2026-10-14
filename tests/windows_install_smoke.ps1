# Exercise the shipped BAT against an actual OptiScaler PE and a foreign Windows DLL.
param([string]$OptiScalerDll = "$PSScriptRoot\..\x64\Release\a\OptiScaler.dll")
$ErrorActionPreference = 'Stop'
$fixture = Join-Path $env:TEMP ("OptiScaler install 'spaces & punctuation' " + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $fixture | Out-Null
$env:CI = 'true'

function Assert([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw $Message }
}
function Run-Batch([string]$Name, [string[]]$Answers, [int]$ExpectedExit = 0) {
    $info = [Diagnostics.ProcessStartInfo]::new()
    $info.FileName = $env:ComSpec
    $info.Arguments = '/d /c ""' + (Join-Path $fixture $Name) + '""'
    $info.WorkingDirectory = $fixture
    $info.UseShellExecute = $false
    $info.RedirectStandardInput = $true
    $info.RedirectStandardOutput = $true
    $info.RedirectStandardError = $true
    $process = [Diagnostics.Process]::Start($info)
    foreach ($answer in $Answers) { $process.StandardInput.WriteLine($answer) }
    $process.StandardInput.Close()
    $outputTask = $process.StandardOutput.ReadToEndAsync()
    $errorTask = $process.StandardError.ReadToEndAsync()
    if (-not $process.WaitForExit(30000)) { $process.Kill(); throw "BAT timed out: $Name" }
    $output = $outputTask.GetAwaiter().GetResult()
    $errorOutput = $errorTask.GetAwaiter().GetResult()
    Assert ($process.ExitCode -eq $ExpectedExit) "$Name exited $($process.ExitCode), expected $ExpectedExit. $output $errorOutput"
    Write-Host $output
}
function Reset-Setup {
    Copy-Item -LiteralPath "$PSScriptRoot\..\setup_windows.bat" -Destination "$fixture\setup_windows.bat"
    Copy-Item -LiteralPath $OptiScalerDll -Destination "$fixture\OptiScaler.dll"
}
try {
    Assert ((Get-Item -LiteralPath $OptiScalerDll).VersionInfo.OriginalFilename -eq 'OptiScaler.dll') 'A compiled OptiScaler DLL is required.'
    Copy-Item -LiteralPath "$env:SystemRoot\System32\version.dll" -Destination "$fixture\dxgi.dll"
    $foreignHash = (Get-FileHash -LiteralPath "$fixture\dxgi.dll").Hash
    [IO.File]::WriteAllText("$fixture\OptiScaler.ini", '[test] configuration must remain unchanged')
    $configHash = (Get-FileHash -LiteralPath "$fixture\OptiScaler.ini").Hash

    # A collision must keep the foreign proxy; setup can choose a different supported filename.
    Reset-Setup
    Run-Batch 'setup_windows.bat' @('1', '2')
    Assert (Test-Path -LiteralPath "$fixture\winmm.dll") 'OptiScaler was not installed as winmm.dll.'
    Assert (-not (Test-Path -LiteralPath "$fixture\setup_windows.bat")) 'Successful setup did not remove itself.'
    Assert (Test-Path -LiteralPath "$fixture\Remove_OptiScaler.bat") 'Maintenance BAT was not created.'
    Assert ((Get-FileHash -LiteralPath "$fixture\dxgi.dll").Hash -eq $foreignHash) 'Setup overwrote a foreign DLL.'

    # The maintenance rename must also reject the foreign target, then accept another proxy.
    Run-Batch 'Remove_OptiScaler.bat' @('2', '1', '3')
    Assert (Test-Path -LiteralPath "$fixture\version.dll") 'Maintenance rename failed.'
    Assert (-not (Test-Path -LiteralPath "$fixture\winmm.dll")) 'The old proxy remained after rename.'
    Run-Batch 'Remove_OptiScaler.bat' @('2', '..\outside.dll', '0')
    Assert (Test-Path -LiteralPath "$fixture\version.dll") 'Invalid filename or cancellation changed the installation.'

    Run-Batch 'Remove_OptiScaler.bat' @('1')
    Assert (-not (Test-Path -LiteralPath "$fixture\version.dll")) 'Maintenance did not delete the verified OptiScaler DLL.'
    Assert (-not (Test-Path -LiteralPath "$fixture\Remove_OptiScaler.bat")) 'Maintenance did not remove itself after the final DLL was removed.'
    Assert ((Get-FileHash -LiteralPath "$fixture\dxgi.dll").Hash -eq $foreignHash) 'Maintenance removed or modified another mod.'
    Assert ((Get-FileHash -LiteralPath "$fixture\OptiScaler.ini").Hash -eq $configHash) 'Maintenance changed settings.'

    Reset-Setup
    Run-Batch 'setup_windows.bat' @('0')
    Assert (Test-Path -LiteralPath "$fixture\setup_windows.bat") 'Cancelled setup removed itself.'
    Assert (Test-Path -LiteralPath "$fixture\OptiScaler.dll") 'Cancelled setup changed the DLL.'
    [IO.File]::WriteAllText("$fixture\OptiScaler.sln", 'source checkout guard')
    Run-Batch 'setup_windows.bat' @() 1
    Assert (Test-Path -LiteralPath "$fixture\setup_windows.bat") 'Source checkout guard removed setup.'
    Remove-Item -LiteralPath "$fixture\OptiScaler.sln"

    # Simulate a game/antivirus locking the source. A failed rename must keep setup and roll back maintenance.
    $locked = [IO.File]::Open("$fixture\OptiScaler.dll", [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read)
    try { Run-Batch 'setup_windows.bat' @('2') 1 } finally { $locked.Dispose() }
    Assert (Test-Path -LiteralPath "$fixture\setup_windows.bat") 'Failed setup removed itself.'
    Assert (-not (Test-Path -LiteralPath "$fixture\Remove_OptiScaler.bat")) 'Failed setup left a new maintenance script behind.'
    Write-Host 'Windows install, rename, collision, cancellation, removal and failure rollback passed.'
} finally {
    Remove-Item -LiteralPath $fixture -Recurse -Force
}
