param(
    [string]$SourceRoot = $PSScriptRoot,
    [string]$StreamlineDirectory = "$PSScriptRoot\.dependencies\streamline\verified\streamline",
    [string]$OutputPath = "$PSScriptRoot\OptiScaler\EmbeddedNotices.txt"
)
$ErrorActionPreference = 'Stop'

# Keep all text in the DLL, independent of loose notice files in a game folder.
# Use ordinal ordering, LF and UTF-8 without a BOM; paths/timestamps are not added.
$inputs = [Collections.Generic.SortedDictionary[string, string]]::new([StringComparer]::Ordinal)
function Add-NoticeFile([string]$LogicalPath, [string]$FullPath) {
    if (-not [IO.File]::Exists($FullPath)) {
        throw "Required license/notice input is missing: $LogicalPath. Initialize submodules and prepare the verified Streamline runtime before building."
    }
    $inputs[$LogicalPath.Replace('\', '/')] = [IO.Path]::GetFullPath($FullPath)
}

function Add-NoticeDirectory([string]$LogicalDirectory, [string]$Directory, [bool]$TextOnly) {
    $fullDirectory = [IO.Path]::GetFullPath($Directory)
    if (-not [IO.Directory]::Exists($fullDirectory)) {
        throw "Required license/notice directory is missing: $LogicalDirectory"
    }
    $count = 0
    foreach ($file in [IO.Directory]::EnumerateFiles($fullDirectory, '*', [IO.SearchOption]::AllDirectories)) {
        $extension = [IO.Path]::GetExtension($file).ToLowerInvariant()
        if ($TextOnly -and $extension -notin @('.txt', '.md')) { continue }
        $relative = $file.Substring($fullDirectory.Length).TrimStart([char[]]@('\', '/')).Replace('\', '/')
        Add-NoticeFile "$LogicalDirectory/$relative" $file
        $count++
    }
    if ($count -eq 0) { throw "Required license/notice directory is empty: $LogicalDirectory" }
}

Add-NoticeFile 'LICENSE' (Join-Path $SourceRoot 'LICENSE')
Add-NoticeDirectory 'Licenses' (Join-Path $SourceRoot 'Licenses') $false
foreach ($relative in @(
    'external/xess/LICENSE.txt',
    'external/FidelityFX-SDK/docs/license.md',
    'external/FidelityFX-SDK-v2/docs/license.md',
    'external/directx_agility_sdk/LICENSE.txt'
)) {
    Add-NoticeFile $relative (Join-Path $SourceRoot $relative)
}

$sm86ManifestPath = Join-Path $SourceRoot 'redist/sm86/manifest.json'
Add-NoticeFile 'redist/sm86/manifest.json' $sm86ManifestPath
$sm86Manifest = [IO.File]::ReadAllText($sm86ManifestPath) | ConvertFrom-Json
foreach ($entry in $sm86Manifest.files) {
    if ([IO.Path]::GetExtension($entry.path).ToLowerInvariant() -in @('.txt', '.md')) {
        Add-NoticeFile "redist/sm86/$($entry.path)" (Join-Path $SourceRoot "redist/sm86/$($entry.path)")
    }
}

# Require every official notice from the pinned SDK, then include any additional
# .txt/.md supplied in its verified staging directory as well.
$streamlineManifestPath = Join-Path $SourceRoot 'redist/streamline/manifest.json'
Add-NoticeFile 'redist/streamline/manifest.json' $streamlineManifestPath
$streamlineManifest = [IO.File]::ReadAllText($streamlineManifestPath) | ConvertFrom-Json
foreach ($entry in $streamlineManifest.files) {
    if ([IO.Path]::GetExtension($entry.name).ToLowerInvariant() -in @('.txt', '.md')) {
        Add-NoticeFile "Streamline/$($entry.name)" (Join-Path $StreamlineDirectory $entry.name)
    }
}
Add-NoticeDirectory 'Streamline' $StreamlineDirectory $true

$text = [Text.StringBuilder]::new()
[void]$text.Append("OptiScaler licenses and third-party notices`n")
[void]$text.Append("Full notices embedded in this DLL; view through Misc > Licenses.`n")
foreach ($entry in $inputs.GetEnumerator()) {
    [void]$text.Append("`n================================================================`n")
    [void]$text.Append($entry.Key).Append("`n================================================================`n")
    $body = [IO.File]::ReadAllText($entry.Value).Replace("`r`n", "`n").Replace("`r", "`n")
    [void]$text.Append($body.TrimEnd([char[]]@("`r", "`n"))).Append("`n")
}

$utf8 = [Text.UTF8Encoding]::new($false)
function Write-ChangedText([string]$Path, [string]$Content) {
    $parent = [IO.Path]::GetDirectoryName([IO.Path]::GetFullPath($Path))
    [void][IO.Directory]::CreateDirectory($parent)
    if (-not [IO.File]::Exists($Path) -or [IO.File]::ReadAllText($Path) -cne $Content) {
        [IO.File]::WriteAllText($Path, $Content, $utf8)
    }
}
Write-ChangedText $OutputPath $text.ToString()
$escaped = [IO.Path]::GetFullPath($OutputPath).Replace('\', '\\').Replace('"', '\"')
$header = "#define EMBEDDED_NOTICES_FILE `"$escaped`"`n"
Write-ChangedText (Join-Path $SourceRoot 'OptiScaler/EmbeddedNoticesPath.h') $header
Write-Host "Embedded $($inputs.Count) complete license/notice texts in $OutputPath"
