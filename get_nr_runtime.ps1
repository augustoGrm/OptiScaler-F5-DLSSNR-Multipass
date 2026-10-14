param(
    [string]$SourceDirectory = "$PSScriptRoot\redist\nr"
)
$ErrorActionPreference = 'Stop'

function Get-NrSha256([string]$Path) {
    $algorithm = [Security.Cryptography.SHA256]::Create()
    $stream = [IO.File]::OpenRead($Path)
    try {
        return [BitConverter]::ToString($algorithm.ComputeHash($stream)).Replace('-', '').ToLowerInvariant()
    } finally {
        $stream.Dispose()
        $algorithm.Dispose()
    }
}

function Test-NrInput([string]$Path, $Entry) {
    return (Test-Path -LiteralPath $Path -PathType Leaf) -and
        (Get-Item -LiteralPath $Path).Length -eq $Entry.size -and
        (Get-NrSha256 $Path) -eq $Entry.sha256
}

function Restore-NrModel([string]$Path, $Entry) {
    if (Test-NrInput $Path $Entry) { return }
    if (-not $Entry.parts -or $Entry.parts.Count -ne 6) {
        throw "Pinned NR input is missing or invalid: $Path"
    }

    $temporary = "$Path.$([Guid]::NewGuid().ToString('N')).tmp"
    $output = $null
    try {
        $output = [IO.File]::Open($temporary, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write)
        $buffer = New-Object byte[] 65536
        foreach ($part in $Entry.parts) {
            $partPath = Join-Path $SourceDirectory $part.path
            if (-not (Test-Path -LiteralPath $partPath -PathType Leaf) -or
                (Get-Item -LiteralPath $partPath).Length -ne $part.size) {
                throw "NR build-input part is missing or has the wrong size: $($part.path)"
            }
            $input = [IO.File]::OpenRead($partPath)
            $algorithm = [Security.Cryptography.SHA256]::Create()
            try {
                while (($count = $input.Read($buffer, 0, $buffer.Length)) -gt 0) {
                    $output.Write($buffer, 0, $count)
                    [void]$algorithm.TransformBlock($buffer, 0, $count, $buffer, 0)
                }
                [void]$algorithm.TransformFinalBlock([byte[]]@(), 0, 0)
                $digest = [BitConverter]::ToString($algorithm.Hash).Replace('-', '').ToLowerInvariant()
                if ($digest -ne $part.sha256) {
                    throw "NR build-input part checksum mismatch: $($part.path)"
                }
            } finally {
                $input.Dispose()
                $algorithm.Dispose()
            }
        }
        $output.Dispose()
        $output = $null
        if (-not (Test-NrInput $temporary $Entry)) {
            throw 'Reconstructed NR model does not match the original supplied DLL.'
        }
        Move-Item -LiteralPath $temporary -Destination $Path -Force
    } finally {
        if ($null -ne $output) { $output.Dispose() }
        if (Test-Path -LiteralPath $temporary) { Remove-Item -LiteralPath $temporary }
    }
}

# These six checked-in build parts reproduce the exact supplied model. No LFS,
# external download URL or different model is needed to build from a checkout.
$manifest = Get-Content -LiteralPath "$PSScriptRoot\redist\nr\manifest.json" -Raw | ConvertFrom-Json
$definitions = @()
foreach ($entry in $manifest.files) {
    $source = [IO.Path]::GetFullPath((Join-Path $SourceDirectory $entry.path))
    if ($entry.role -eq 'model') { Restore-NrModel $source $entry }
    if (-not (Test-NrInput $source $entry)) {
        throw "NR checksum/size mismatch: $($entry.path). Restore the pinned files from the repository."
    }
    $escaped = $source.Replace('\', '\\')
    $definitions += "#define $($entry.resource_macro) `"$escaped`""
}
$resourceHeader = "$PSScriptRoot\OptiScaler\dlssnr\NrRuntimePath.h"
$resourceText = ($definitions -join "`r`n") + "`r`n"
if (-not (Test-Path -LiteralPath $resourceHeader) -or
    (Get-Content -LiteralPath $resourceHeader -Raw) -ne $resourceText) {
    [IO.File]::WriteAllText($resourceHeader, $resourceText, [Text.Encoding]::ASCII)
}
Write-Host "Verified embedded NR $($manifest.version) forwarder and model: $SourceDirectory"
