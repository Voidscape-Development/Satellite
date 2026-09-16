<#
.SYNOPSIS
    Builds the OMT libraries Satellite ships: libomt and libvmx.

.DESCRIPTION
    Upstream publishes no binaries for either - both are source-only, and libomt is C# that
    has to be compiled to a native library with .NET NativeAOT. So Satellite builds them,
    and this is the Windows counterpart to build-aux/build-omt. The .NET requirement is
    build-time only; nothing at run time needs a .NET installation.

    Requires the .NET 8 SDK, clang++ on PATH, and git.

.PARAMETER OutputDirectory
    Where the finished libomt.dll and libvmx.dll are written. Defaults to deps/omt.
#>

[CmdletBinding()]
param(
    [string] $OutputDirectory
)

$ErrorActionPreference = 'Stop'

$RootDir = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path

if (-not $OutputDirectory) {
    $OutputDirectory = Join-Path $RootDir 'deps/omt'
}

# Deliberately outside the source tree, matching build-omt: the macOS build sweeps .deps with
# xattr and read-only git pack files fail it, and there is no reason for the platforms to
# differ on where scratch clones live.
$WorkDir = if ($env:OMT_BUILD_DIR) { $env:OMT_BUILD_DIR } else { Join-Path $env:TEMP 'satellite-omt-build' }

# Pinned in buildspec.json so an update is a reviewable diff. Keep in step with build-omt.
$Spec = Get-Content (Join-Path $RootDir 'buildspec.json') -Raw | ConvertFrom-Json
$Pins = @{
    libomtnet = $Spec.omt.libomtnetCommit
    libomt    = $Spec.omt.libomtCommit
    libvmx    = $Spec.omt.libvmxCommit
}

foreach ($name in $Pins.Keys) {
    if (-not $Pins[$name]) {
        throw "could not read the $name commit pin from buildspec.json"
    }
}

Write-Host "==> building the OMT libraries for windows into $OutputDirectory"

New-Item -ItemType Directory -Force -Path $WorkDir, $OutputDirectory | Out-Null

# libomt references libomtnet by a relative HintPath, so the two have to be siblings.
function Get-Source {
    param([string] $Name, [string] $Commit)

    $dir = Join-Path $WorkDir $Name
    if (-not (Test-Path (Join-Path $dir '.git'))) {
        Write-Host "==> cloning $Name"
        git clone --quiet "https://github.com/openmediatransport/$Name" $dir
        if ($LASTEXITCODE -ne 0) { throw "failed to clone $Name" }
    }

    git -C $dir fetch --quiet origin
    git -C $dir checkout --quiet $Commit
    if ($LASTEXITCODE -ne 0) { throw "failed to check out $Commit in $Name" }

    $short = git -C $dir rev-parse --short HEAD
    Write-Host "    $Name at $short"
}

Get-Source -Name 'libomtnet' -Commit $Pins.libomtnet
Get-Source -Name 'libomt' -Commit $Pins.libomt
Get-Source -Name 'libvmx' -Commit $Pins.libvmx

Write-Host '==> building libvmx'
# -Wno-c++11-narrowing: libvmx initialises byte arrays from negative literals, which older
# clang only warned about and current clang rejects. Upstream's own script has no such flag
# and does not compile as-is with clang 18+.
$vmxSrc = Join-Path $WorkDir 'libvmx/src'
$vmxDef = Join-Path $WorkDir 'libvmx/exports.def'
$vmxOut = Join-Path $OutputDirectory 'libvmx.dll'

& clang++ -O3 -std=c++17 -fdeclspec -mlzcnt -mavx2 -mbmi -Wno-c++11-narrowing -shared `
    "-Wl,/DEF:$vmxDef" `
    (Join-Path $vmxSrc 'vmxcodec_x86.cpp') `
    (Join-Path $vmxSrc 'vmxcodec_avx2.cpp') `
    (Join-Path $vmxSrc 'vmxcodec.cpp') `
    -o $vmxOut
if ($LASTEXITCODE -ne 0) { throw 'libvmx build failed' }

Write-Host '==> building libomtnet'
& dotnet build (Join-Path $WorkDir 'libomtnet/libomtnet.sln') -c Release --nologo -v quiet
if ($LASTEXITCODE -ne 0) { throw 'libomtnet build failed' }

Write-Host '==> building libomt (NativeAOT)'
& dotnet publish (Join-Path $WorkDir 'libomt/libomt.sln') -r win-x64 -c Release --nologo -v quiet
if ($LASTEXITCODE -ne 0) { throw 'libomt build failed' }

Copy-Item (Join-Path $WorkDir 'libomt/bin/Release/net8.0/win-x64/native/libomt.dll') `
    (Join-Path $OutputDirectory 'libomt.dll') -Force

Write-Host '==> done'
Get-ChildItem $OutputDirectory
