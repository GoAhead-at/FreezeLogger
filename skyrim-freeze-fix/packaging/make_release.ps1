# WorkerSpinLockFix release packager.
#
# Builds Release/x64 via the windows-x64-release CMake preset, stages the
# Mod Organizer 2 directory layout, and packs it into a single RAR archive.
#
# Usage:
#   .\packaging\make_release.ps1
#   .\packaging\make_release.ps1 -SkipBuild
#   .\packaging\make_release.ps1 -RarExe 'C:\Program Files\WinRAR\Rar.exe'

[CmdletBinding()]
param(
    [switch] $SkipBuild,
    [string] $RarExe,
    [string] $OutputDir = (Join-Path $PSScriptRoot '..\dist-out')
)

$ErrorActionPreference = 'Stop'

function Get-PluginVersion {
    param([string] $CMakeListsPath)
    $content = Get-Content -Raw $CMakeListsPath
    if ($content -match 'project\(\s*WorkerSpinLockFix[^\)]*VERSION\s+(\d+\.\d+\.\d+)') {
        return $matches[1]
    }
    throw "Could not parse VERSION from $CMakeListsPath"
}

function Resolve-RarTool {
    param([string] $Override)
    if ($Override) {
        if (-not (Test-Path $Override)) { throw "Specified -RarExe not found: $Override" }
        return @{ Path = $Override }
    }
    $candidates = @(
        'C:\Program Files\WinRAR\Rar.exe',
        'C:\Program Files (x86)\WinRAR\Rar.exe'
    ) | Where-Object { Test-Path $_ }
    if ($candidates) { return @{ Path = $candidates | Select-Object -First 1 } }
    $cmd = Get-Command 'rar.exe' -ErrorAction SilentlyContinue
    if ($cmd) { return @{ Path = $cmd.Source } }
    throw "No RAR tool found. Install WinRAR, put rar.exe on PATH, or pass -RarExe."
}

$repoRoot   = Resolve-Path (Join-Path $PSScriptRoot '..')
$cmakeLists = Join-Path $repoRoot 'CMakeLists.txt'
$version    = Get-PluginVersion $cmakeLists

$preset    = 'windows-x64-release'
# CMakePresets.json points binaryDir to C:\sk\wslf\r to keep MSVC PDB
# paths under MAX_PATH. The DLL/PDB live under the Release/ subfolder.
$buildDir   = 'C:\sk\wslf\r'
$dllPath    = Join-Path $buildDir 'Release\WorkerSpinLockFix.dll'
$pdbPath    = Join-Path $buildDir 'Release\WorkerSpinLockFix.pdb'
$tomlPath   = Join-Path $repoRoot 'dist\WorkerSpinLockFix.toml'
$stageDir   = Join-Path $repoRoot 'build\stage'
$pluginsDir = Join-Path $stageDir 'SKSE\Plugins'
$rarBaseName = "WorkerSpinLockFix_v$version.rar"
$rarPath    = Join-Path $OutputDir $rarBaseName

Write-Host "WorkerSpinLockFix v$version (preset: $preset)" -ForegroundColor Cyan

if (-not $SkipBuild) {
    if (-not $env:VCPKG_ROOT) {
        $env:VCPKG_ROOT = 'd:/Programme/Microsoft Visual Studio/18/Community/VC/vcpkg'
    }
    $cmake = 'd:/Programme/Microsoft Visual Studio/18/Community/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe'

    Write-Host '== Configure =='
    & $cmake --preset $preset
    if ($LASTEXITCODE -ne 0) { throw 'CMake configure failed.' }

    Write-Host '== Build =='
    & $cmake --build $buildDir --config Release
    if ($LASTEXITCODE -ne 0) { throw 'CMake build failed.' }
}

if (-not (Test-Path $dllPath))  { throw "Missing build artifact: $dllPath" }
if (-not (Test-Path $tomlPath)) { throw "Missing build artifact: $tomlPath" }
if (-not (Test-Path $pdbPath)) {
    Write-Warning "No PDB found at $pdbPath."
}

Write-Host '== Stage =='
if (Test-Path $stageDir) { Remove-Item -Recurse -Force $stageDir }
New-Item -ItemType Directory -Force -Path $pluginsDir | Out-Null
Copy-Item $dllPath  $pluginsDir
if (Test-Path $pdbPath) { Copy-Item $pdbPath $pluginsDir }
Copy-Item $tomlPath $pluginsDir

Write-Host '== Pack =='
New-Item -ItemType Directory -Force -Path $OutputDir | Out-Null
if (Test-Path $rarPath) { Remove-Item -Force $rarPath }

$rar = Resolve-RarTool -Override $RarExe
Write-Host ("Using RAR tool: " + $rar.Path)

Push-Location $stageDir
try {
    & $rar.Path a -r -ep1 $rarPath 'SKSE'
    if ($LASTEXITCODE -ne 0) { throw "RAR exited with $LASTEXITCODE." }
} finally {
    Pop-Location
}

Write-Host ''
Write-Host "Packaged: $rarPath" -ForegroundColor Green
Write-Host 'Contents:'
& $rar.Path lb $rarPath
