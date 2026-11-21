<#
.SYNOPSIS
    Local Windows build helper for obs-chat-overlay.

.DESCRIPTION
    Downloads the OBS plugin template scaffolding (cmake modules + presets),
    configures, builds and installs the plugin into .\release\<Config>.
    Requires: Visual Studio 2022/2026 with the C++ workload, CMake >= 3.28,
    git and PowerShell 7.

.PARAMETER Configuration
    Debug, RelWithDebInfo, Release or MinSizeRel.
#>
[CmdletBinding()]
param(
    [ValidateSet('Debug', 'RelWithDebInfo', 'Release', 'MinSizeRel')]
    [string] $Configuration = 'RelWithDebInfo'
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
Push-Location $root
try {
    $scaffolding = Join-Path $root '_scaffolding'
    if (!(Test-Path $scaffolding)) {
        Write-Host 'Fetching OBS plugin template scaffolding...'
        git clone --quiet https://github.com/obsproject/obs-plugintemplate.git $scaffolding
        git -C $scaffolding checkout --quiet 3e7d7ac3b5342cd7d9b88890b9c70b472d1520fc
    }

    Copy-Item -Recurse -Force (Join-Path $scaffolding 'cmake') $root
    Copy-Item -Force (Join-Path $scaffolding 'CMakePresets.json') $root

    Write-Host "Configuring ($Configuration)..."
    cmake --preset windows-ci-x64
    if ($LASTEXITCODE -ne 0) { throw 'cmake configure failed' }

    Write-Host "Building ($Configuration)..."
    cmake --build --preset windows-x64 --config $Configuration --parallel
    if ($LASTEXITCODE -ne 0) { throw 'cmake build failed' }

    $prefix = Join-Path $root "release/$Configuration"
    Write-Host "Installing to $prefix..."
    cmake --install build_x64 --prefix $prefix --config $Configuration
    if ($LASTEXITCODE -ne 0) { throw 'cmake install failed' }

    # Rebuild the classic OBS layout that OBS 28-33 load.
    $stage = Join-Path $root 'stage'
    Remove-Item -Recurse -Force $stage -ErrorAction SilentlyContinue
    $dll = Get-ChildItem -Path $prefix -Recurse -Filter obs-chat-overlay.dll | Select-Object -First 1
    if (-not $dll) { throw 'obs-chat-overlay.dll was not produced' }
    New-Item -ItemType Directory -Force (Join-Path $stage 'obs-plugins/64bit') | Out-Null
    Copy-Item $dll.FullName (Join-Path $stage 'obs-plugins/64bit/')

    $locale = Get-ChildItem -Path $prefix -Recurse -Directory -Filter locale | Select-Object -First 1
    if ($locale) {
        $destination = Join-Path $stage 'data/obs-plugins/obs-chat-overlay'
        New-Item -ItemType Directory -Force $destination | Out-Null
        Copy-Item -Path (Join-Path $locale.Parent.FullName '*') -Destination $destination -Recurse -Force
    }

    Compress-Archive -Path (Join-Path $stage '*') -DestinationPath (Join-Path $root 'obs-chat-overlay-windows-x64.zip') -Force
    Write-Host "Done: $prefix"
    Write-Host "Ready-to-install archive: $(Join-Path $root 'obs-chat-overlay-windows-x64.zip')"
} finally {
    Pop-Location
}
