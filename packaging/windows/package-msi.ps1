# SPDX-License-Identifier: MPL-2.0

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string] $BuildDirectory,

    [Parameter(Mandatory = $true)]
    [string] $StagingDirectory,

    [Parameter(Mandatory = $true)]
    [string] $OutputDirectory,

    [ValidatePattern('^[0-9]+\.[0-9]+\.[0-9]+$')]
    [string] $Version = '0.1.0',

    [ValidateSet('Release', 'RelWithDebInfo')]
    [string] $Configuration = 'Release'
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Resolve-FullPath([string] $Path) {
    return [System.IO.Path]::GetFullPath($Path)
}

$scriptDirectory = Resolve-FullPath $PSScriptRoot
$buildRoot = Resolve-FullPath $BuildDirectory
$stageRoot = Resolve-FullPath $StagingDirectory
$artifactRoot = Resolve-FullPath $OutputDirectory
$wixSource = Join-Path $scriptDirectory 'Product.wxs'

if (-not (Test-Path -LiteralPath $buildRoot -PathType Container)) {
    throw "CMake build directory does not exist: $buildRoot"
}

if (Test-Path -LiteralPath $stageRoot) {
    $existingEntry = Get-ChildItem -LiteralPath $stageRoot -Force | Select-Object -First 1
    if ($null -ne $existingEntry) {
        throw "Staging directory must be empty; refusing to overwrite: $stageRoot"
    }
} else {
    New-Item -ItemType Directory -Path $stageRoot | Out-Null
}

if (Test-Path -LiteralPath $artifactRoot) {
    $existingArtifact = Get-ChildItem -LiteralPath $artifactRoot -Force | Select-Object -First 1
    if ($null -ne $existingArtifact) {
        throw "Output directory must be empty; refusing to overwrite: $artifactRoot"
    }
} else {
    New-Item -ItemType Directory -Path $artifactRoot | Out-Null
}

if ($null -eq (Get-Command cmake -ErrorAction SilentlyContinue)) {
    throw 'cmake was not found on PATH.'
}
if ($null -eq (Get-Command wix -ErrorAction SilentlyContinue)) {
    throw 'WiX 5 or newer was not found on PATH.'
}
$wixVersion = (& wix --version | Select-Object -Last 1).Trim()
$wixVersionMatch = [regex]::Match($wixVersion, '^(?<major>[0-9]+)\.')
if ($LASTEXITCODE -ne 0 -or -not $wixVersionMatch.Success) {
    throw "Unable to determine the WiX version: $wixVersion"
}
if ([int]($wixVersionMatch.Groups['major'].Value) -lt 5) {
    throw "WiX 5 or newer is required; found $wixVersion"
}

& cmake --install $buildRoot --prefix $stageRoot --config $Configuration
if ($LASTEXITCODE -ne 0) {
    throw "cmake --install failed with exit code $LASTEXITCODE"
}

$application = Join-Path $stageRoot 'bin\VideoEditor.exe'
if (-not (Test-Path -LiteralPath $application -PathType Leaf)) {
    throw "The install tree does not contain bin\VideoEditor.exe: $stageRoot"
}

$fileManifest = foreach ($file in Get-ChildItem -LiteralPath $stageRoot -File -Recurse) {
    $relativePath = [System.IO.Path]::GetRelativePath($stageRoot, $file.FullName)
    $hash = Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256
    [ordered]@{
        path = $relativePath.Replace('\', '/')
        bytes = $file.Length
        sha256 = $hash.Hash.ToLowerInvariant()
    }
}
$manifestPath = Join-Path $artifactRoot 'installed-files.json'
ConvertTo-Json -InputObject @($fileManifest) -Depth 3 |
    Set-Content -LiteralPath $manifestPath -Encoding utf8

$msiPath = Join-Path $artifactRoot "VideoEditor-$Version-x64.msi"
& wix build $wixSource `
    -arch x64 `
    -d "ProductVersion=$Version" `
    -d "InstallRoot=$stageRoot" `
    -o $msiPath
if ($LASTEXITCODE -ne 0) {
    throw "wix build failed with exit code $LASTEXITCODE"
}

Write-Host "Created unsigned MSI: $msiPath"
Write-Warning 'Code signing is not configured. Do not publish this MSI as an official build.'
