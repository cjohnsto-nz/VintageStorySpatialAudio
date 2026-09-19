#Requires -Version 7.0
<#
.SYNOPSIS
    Builds the mod and installs it into your local VintagestoryData/Mods folder.
.PARAMETER NoBuild
    Install the zip already in artifacts/ without rebuilding.
.PARAMETER Configuration
    Release (the default: the engine is a real-time renderer, and Debug runs several times slower)
    or Debug.
.PARAMETER RunTests
    Run native and managed tests as part of the build (off by default for fast iteration).
.PARAMETER StopGame
    Stop a running Vintage Story before installing.
.PARAMETER LaunchGame
    Start Vintage Story after installing (uses VINTAGE_STORY or the default install).
#>
param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release',
    [switch]$NoBuild,
    [switch]$RunTests,
    [switch]$StopGame,
    [switch]$LaunchGame
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$Root = $PSScriptRoot

if (-not $NoBuild) {
    & (Join-Path $Root 'scripts/build.ps1') -Configuration $Configuration -SkipTests:(-not $RunTests)
}

$modInfo = Get-Content -Raw (Join-Path $Root 'src/VintageStorySteamAudio/modinfo.json') | ConvertFrom-Json
$zip = Join-Path $Root "artifacts/$($modInfo.modid)_$($modInfo.version).zip"
if (-not (Test-Path $zip)) { throw "Package not found: $zip (run without -NoBuild)" }

$dataDir =
    if ($IsWindows) { Join-Path $env:APPDATA 'VintagestoryData' }
    elseif ($IsMacOS) { Join-Path $HOME 'Library/Application Support/VintagestoryData' }
    else { Join-Path $HOME '.config/VintagestoryData' }
$modsDir = Join-Path $dataDir 'Mods'

if ($StopGame) {
    $running = Get-Process -Name 'Vintagestory' -ErrorAction SilentlyContinue
    if ($running) {
        Write-Host 'Stopping Vintage Story...'
        $running | Stop-Process -Force
        $running | Wait-Process -Timeout 15 -ErrorAction SilentlyContinue
    }
}

New-Item -ItemType Directory -Force -Path $modsDir | Out-Null
Get-ChildItem -Path $modsDir -Filter "$($modInfo.modid)_*.zip" | Remove-Item -Force
Copy-Item $zip $modsDir
Write-Host "Installed $(Split-Path $zip -Leaf) into $modsDir" -ForegroundColor Green

if ($LaunchGame) {
    $game = $env:VINTAGE_STORY
    if (-not $game -and $IsWindows) { $game = Join-Path $env:APPDATA 'Vintagestory' }
    $exe = if ($IsWindows) { Join-Path $game 'Vintagestory.exe' } else { Join-Path $game 'Vintagestory' }
    if (-not (Test-Path $exe)) { throw "Game executable not found at $exe; set VINTAGE_STORY." }
    Start-Process -FilePath $exe
}
