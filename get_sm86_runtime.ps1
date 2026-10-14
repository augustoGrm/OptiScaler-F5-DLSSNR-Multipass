param(
    [string]$Destination = "$PSScriptRoot\redist\sm86",
    [string]$SourceDirectory
)
$ErrorActionPreference = 'Stop'
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
$manifest = Get-Content -LiteralPath "$PSScriptRoot\redist\sm86\manifest.json" -Raw | ConvertFrom-Json
New-Item -ItemType Directory -Force -Path $Destination | Out-Null
foreach ($entry in $manifest.files) {
    $target = Join-Path $Destination $entry.path
    if ((Test-Path -LiteralPath $target) -and
        (Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash -eq $entry.sha256) { continue }
    $temp = "$target.$([Guid]::NewGuid().ToString('N')).tmp"
    try {
        if ($SourceDirectory) {
            Copy-Item -LiteralPath (Join-Path $SourceDirectory $entry.path) -Destination $temp
        } else {
            $url = "https://raw.githubusercontent.com/$($manifest.repository)/$($manifest.commit)/$($entry.path)"
            Invoke-WebRequest -Uri $url -OutFile $temp -UseBasicParsing
        }
        if ((Get-FileHash -LiteralPath $temp -Algorithm SHA256).Hash -ne $entry.sha256) {
            throw "SM86 checksum mismatch: $($entry.path)"
        }
        Move-Item -LiteralPath $temp -Destination $target -Force
    } finally {
        if (Test-Path -LiteralPath $temp) { Remove-Item -LiteralPath $temp }
    }
}
$runtimeFile = [IO.Path]::GetFullPath((Join-Path $Destination 'version.dll')).Replace('\', '\\')
$resourceHeader = "$PSScriptRoot\OptiScaler\framegen\dlssg\Sm86RuntimePath.h"
$resourceText = "#define SM86_RUNTIME_FILE `"$runtimeFile`"`r`n"
if (-not (Test-Path -LiteralPath $resourceHeader) -or
    (Get-Content -LiteralPath $resourceHeader -Raw) -ne $resourceText) {
    [IO.File]::WriteAllText($resourceHeader, $resourceText, [Text.Encoding]::ASCII)
}
Write-Host "Verified SM86 $($manifest.version) runtime and licences: $Destination"
