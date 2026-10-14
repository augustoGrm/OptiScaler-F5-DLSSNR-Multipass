param(
    [string]$Destination = "$PSScriptRoot\redist\sm86",
    [string]$SourceDirectory
)
$ErrorActionPreference = 'Stop'

# MSBuild can launch Windows PowerShell with PowerShell 7's PSModulePath.
# Use the .NET API so verification does not depend on Get-FileHash autoloading.
function Get-Sm86Sha256([string]$Path) {
    $algorithm = [Security.Cryptography.SHA256]::Create()
    $stream = [IO.File]::OpenRead($Path)
    try {
        return [BitConverter]::ToString($algorithm.ComputeHash($stream)).Replace('-', '').ToLowerInvariant()
    } finally {
        $stream.Dispose()
        $algorithm.Dispose()
    }
}

[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
$manifest = Get-Content -LiteralPath "$PSScriptRoot\redist\sm86\manifest.json" -Raw | ConvertFrom-Json
New-Item -ItemType Directory -Force -Path $Destination | Out-Null
foreach ($entry in $manifest.files) {
    $target = Join-Path $Destination $entry.path
    if ((Test-Path -LiteralPath $target) -and
        (Get-Sm86Sha256 $target) -eq $entry.sha256) { continue }
    $temp = "$target.$([Guid]::NewGuid().ToString('N')).tmp"
    try {
        if ($SourceDirectory) {
            Copy-Item -LiteralPath (Join-Path $SourceDirectory $entry.path) -Destination $temp
        } else {
            $url = "https://raw.githubusercontent.com/$($manifest.repository)/$($manifest.commit)/$($entry.path)"
            Invoke-WebRequest -Uri $url -OutFile $temp -UseBasicParsing
        }
        if ((Get-Sm86Sha256 $temp) -ne $entry.sha256) {
            throw "SM86 checksum mismatch: $($entry.path)"
        }
        Move-Item -LiteralPath $temp -Destination $target -Force
    } finally {
        if (Test-Path -LiteralPath $temp) { Remove-Item -LiteralPath $temp }
    }
}
# Traverse the PE resource directory without loading either proxy. The pinned
# release contains the NVIDIA runtime and its native backend as nested RCDATA.
function Get-Sm86Resource([byte[]]$Image, [int]$ResourceId) {
    function Read-U16([int]$Offset) {
        if ($Offset -lt 0 -or $Offset + 2 -gt $Image.Length) { throw 'Truncated SM86 PE image' }
        return [BitConverter]::ToUInt16($Image, $Offset)
    }
    function Read-U32([int]$Offset) {
        if ($Offset -lt 0 -or $Offset + 4 -gt $Image.Length) { throw 'Truncated SM86 PE image' }
        return [BitConverter]::ToUInt32($Image, $Offset)
    }
    $peOffset = Read-U32 0x3c
    if ((Read-U16 0) -ne 0x5a4d -or (Read-U32 $peOffset) -ne 0x4550 -or
        (Read-U16 ($peOffset + 4)) -ne 0x8664 -or (Read-U16 ($peOffset + 24)) -ne 0x20b) {
        throw 'SM86 payload is not the expected x64 PE image'
    }
    $sectionCount = Read-U16 ($peOffset + 6)
    $sectionTable = $peOffset + 24 + (Read-U16 ($peOffset + 20))
    function Map-Rva([uint32]$Rva) {
        for ($i = 0; $i -lt $sectionCount; $i++) {
            $section = $sectionTable + $i * 40
            $virtualAddress = Read-U32 ($section + 12)
            $rawSize = Read-U32 ($section + 16)
            if ($Rva -ge $virtualAddress -and ($Rva - $virtualAddress) -lt $rawSize) {
                return (Read-U32 ($section + 20)) + $Rva - $virtualAddress
            }
        }
        throw "Unmapped SM86 PE resource RVA $Rva"
    }
    $root = Map-Rva (Read-U32 ($peOffset + 24 + 112 + 16))
    $directory = $root
    foreach ($key in @(10, $ResourceId)) {
        $count = (Read-U16 ($directory + 12)) + (Read-U16 ($directory + 14))
        $found = @()
        for ($i = 0; $i -lt $count; $i++) {
            $entry = $directory + 16 + $i * 8
            if ((Read-U32 $entry) -eq $key) {
                $value = Read-U32 ($entry + 4)
                if (($value -band 0x80000000L) -eq 0) { throw 'Unexpected SM86 resource leaf' }
                $found += $root + ($value -band 0x7fffffffL)
            }
        }
        if ($found.Count -ne 1) { throw "Missing or ambiguous SM86 RCDATA $ResourceId" }
        $directory = $found[0]
    }
    $languages = (Read-U16 ($directory + 12)) + (Read-U16 ($directory + 14))
    if ($languages -ne 1) { throw "Ambiguous SM86 resource language $ResourceId" }
    $leaf = Read-U32 ($directory + 20)
    if (($leaf -band 0x80000000L) -ne 0) { throw 'Unexpected SM86 resource depth' }
    $dataEntry = $root + $leaf
    $offset = Map-Rva (Read-U32 $dataEntry)
    $size = Read-U32 ($dataEntry + 4)
    if ($size -eq 0 -or $offset + $size -gt $Image.Length) { throw 'Truncated SM86 resource' }
    $bytes = New-Object byte[] $size
    [Buffer]::BlockCopy($Image, $offset, $bytes, 0, $size)
    return ,$bytes
}

foreach ($entry in $manifest.embedded_components) {
    $target = Join-Path $Destination $entry.path
    if ((Test-Path -LiteralPath $target) -and
        (Get-Sm86Sha256 $target) -eq $entry.sha256) { continue }
    [byte[]]$component = [IO.File]::ReadAllBytes((Join-Path $Destination 'version.dll'))
    foreach ($id in $entry.resource_chain) {
        $component = Get-Sm86Resource $component $id
    }
    if ($component.Length -ne $entry.size) { throw "SM86 component size mismatch: $($entry.path)" }
    $temp = "$target.$([Guid]::NewGuid().ToString('N')).tmp"
    try {
        [IO.File]::WriteAllBytes($temp, $component)
        if ((Get-Sm86Sha256 $temp) -ne $entry.sha256) { throw "SM86 component checksum mismatch: $($entry.path)" }
        Move-Item -LiteralPath $temp -Destination $target -Force
    } finally {
        if (Test-Path -LiteralPath $temp) { Remove-Item -LiteralPath $temp }
    }
}
$runtimeFile = [IO.Path]::GetFullPath((Join-Path $Destination 'nvngx_dlssg.dll')).Replace('\', '\\')
$backendFile = [IO.Path]::GetFullPath((Join-Path $Destination 'sm86_backend.dll')).Replace('\', '\\')
$resourceHeader = "$PSScriptRoot\OptiScaler\framegen\dlssg\Sm86RuntimePath.h"
$resourceText = "#define SM86_RUNTIME_FILE `"$runtimeFile`"`r`n#define SM86_BACKEND_FILE `"$backendFile`"`r`n"
if (-not (Test-Path -LiteralPath $resourceHeader) -or
    (Get-Content -LiteralPath $resourceHeader -Raw) -ne $resourceText) {
    [IO.File]::WriteAllText($resourceHeader, $resourceText, [Text.Encoding]::ASCII)
}
Write-Host "Verified and extracted SM86 $($manifest.version) native backend, NVIDIA runtime and licences: $Destination"
