#Requires -Version 7.0
<#
.SYNOPSIS
    Full build of the mod for the current platform: dependencies, native engine, managed mod,
    tests, the VsaDoctor check and the packaged mod zip.
.PARAMETER Configuration
    Debug or Release (default).
.PARAMETER SkipTests
    Skip native and managed tests (the VsaDoctor check still runs).
.PARAMETER SkipNative
    Reuse the natives already in artifacts/native (e.g. natives for all platforms gathered by CI).
.PARAMETER SkipDoctor
    Skip the VsaDoctor check. Only for CI packaging, where the game reference assemblies come
    from the dedicated-server package and cannot load the client types the doctor verifies.
.OUTPUTS
    artifacts/native/<rid>/       native engine + phonon
    artifacts/mod/                 the mod folder (drop-in for VintagestoryData/Mods)
    artifacts/vssteamaudio_<version>.zip
.NOTES
    Requires VINTAGE_STORY (or a default install) for the game reference assemblies.
#>
param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release',
    [switch]$SkipTests,
    [switch]$SkipNative,
    [switch]$SkipDoctor
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$PSNativeCommandUseErrorActionPreference = $true

$Root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$Artifacts = Join-Path $Root 'artifacts'
$NativeArtifacts = Join-Path $Artifacts 'native'

function Step([string]$Message) { Write-Host "`n==> $Message" -ForegroundColor Cyan }

$preset = if ($IsWindows) { 'win-x64' } elseif ($IsMacOS) { 'osx' } else { 'linux-x64' }
$rid = $preset
$buildPreset = "$preset-$($Configuration.ToLowerInvariant())"

Step 'Fetching pinned dependencies'
& (Join-Path $PSScriptRoot 'fetch-deps.ps1')

if (-not $SkipNative) {
    Push-Location (Join-Path $Root 'native')
    try {
        Step "Native engine: configure ($preset)"
        cmake --preset $preset
        Step "Native engine: build ($buildPreset)"
        cmake --build --preset $buildPreset
        if (-not $SkipTests) {
            Step 'Native engine: tests'
            ctest --preset $buildPreset
        }
        Step 'Native engine: install to artifacts/native'
        $ridArtifacts = Join-Path $NativeArtifacts $rid
        if (Test-Path $ridArtifacts) { Remove-Item -Recurse -Force $ridArtifacts }
        cmake --install (Join-Path 'build' $preset) --config $Configuration --prefix $NativeArtifacts
    }
    finally { Pop-Location }
}

$solution = Join-Path $Root 'VintageStorySteamAudio.sln'
if ($SkipTests) {
    # Only the shipped mod and the doctor tool; the test project (and its NuGet restore) is skipped.
    Step "Managed: build mod + VsaDoctor ($Configuration)"
    dotnet build (Join-Path $Root 'tools/VsaDoctor/VsaDoctor.csproj') -c $Configuration -p:RequireNatives=true
}
else {
    Step "Managed: build ($Configuration)"
    dotnet build $solution -c $Configuration -p:RequireNatives=true
    Step 'Managed: tests'
    dotnet test $solution -c $Configuration --no-build
}

if (-not $SkipDoctor) {
    Step 'VsaDoctor: game integration points + native self-test'
    $doctor = Join-Path $Root "tools/VsaDoctor/bin/$Configuration/net10.0/VsaDoctor.dll"
    dotnet $doctor --native (Join-Path $NativeArtifacts $rid)
}

Step 'Packaging'
$modBuild = Join-Path $Root "src/VintageStorySteamAudio/bin/$Configuration/net10.0/mod"
$modArtifact = Join-Path $Artifacts 'mod'
if (Test-Path $modArtifact) { Remove-Item -Recurse -Force $modArtifact }
Copy-Item -Recurse $modBuild $modArtifact

# Licence notices for everything we redistribute (Steam Audio and the components it bundles).
$notices = @(
    '# Third-party notices',
    '',
    'This mod redistributes Steam Audio (Copyright Valve Corporation, Apache License 2.0; the full',
    'Apache 2.0 text is reproduced below) and the components Steam Audio bundles.',
    '',
    (Get-Content -Raw (Join-Path $Root 'third_party/VERSIONS.md')),
    '',
    (Get-Content -Raw (Join-Path $Root 'third_party/steamaudio/THIRDPARTY.md'))
) -join [Environment]::NewLine
Set-Content -Path (Join-Path $modArtifact 'THIRD_PARTY_NOTICES.md') -Value $notices

$modInfo = Get-Content -Raw (Join-Path $modArtifact 'modinfo.json') | ConvertFrom-Json
$zip = Join-Path $Artifacts "$($modInfo.modid)_$($modInfo.version).zip"
if (Test-Path $zip) { Remove-Item -Force $zip }
Compress-Archive -Path (Join-Path $modArtifact '*') -DestinationPath $zip
Write-Host "`nBuilt $zip" -ForegroundColor Green
$platforms = (Get-ChildItem (Join-Path $modArtifact 'native') -Directory | ForEach-Object Name) -join ', '
Write-Host "Native platforms in package: $platforms"
