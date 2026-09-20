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
.PARAMETER Split
    Package for release: the main mod with the Windows libraries, and the Linux and macOS ones
    in a second mod (spatialaudiounix), each under the mod database's 40 MB limit. Needs all
    three platforms' libraries in artifacts/native, which CI has.
.PARAMETER SkipDoctor
    Skip the VsaDoctor check. Only for CI packaging, where the game reference assemblies come
    from the dedicated-server package and cannot load the client types the doctor verifies.
.OUTPUTS
    artifacts/native/<rid>/       native engine + phonon
    artifacts/mod/                 the mod folder (drop-in for VintagestoryData/Mods)
    artifacts/spatialaudio_<version>.zip
.NOTES
    Requires VINTAGE_STORY (or a default install) for the game reference assemblies.
#>
param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release',
    [switch]$SkipTests,
    [switch]$SkipNative,
    [switch]$SkipDoctor,
    [switch]$Split
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

$solution = Join-Path $Root 'VintageStorySpatialAudio.sln'
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

if (-not $SkipTests) {
    # Golden scenes with pass/fail expectations (levels, underruns, render load).
    Step 'SceneLab: render scenarios'
    $sceneLab = Join-Path $Root "tools/SceneLab/bin/$Configuration/net10.0/SceneLab.dll"
    $scenarios = Get-ChildItem (Join-Path $Root 'tools/SceneLab/scenarios') -Filter *.json | ForEach-Object FullName
    dotnet $sceneLab @scenarios --native (Join-Path $NativeArtifacts $rid) --out (Join-Path $Artifacts 'scenelab')
}

Step 'Packaging'
$modBuild = Join-Path $Root "src/VintageStorySpatialAudio/bin/$Configuration/net10.0/mod"
$modArtifact = Join-Path $Artifacts 'mod'
if (Test-Path $modArtifact) { Remove-Item -Recurse -Force $modArtifact }
Copy-Item -Recurse $modBuild $modArtifact

# Licence notices for everything we redistribute: Steam Audio and the components it bundles, and
# libogg/libvorbis (BSD-3-Clause, linked into vsaudio). miniaudio is public domain / MIT-0.
$notices = @(
    '# Third-party notices',
    '',
    'This mod redistributes Steam Audio (Copyright Valve Corporation, Apache License 2.0; the full',
    'Apache 2.0 text is reproduced below) and the components Steam Audio bundles, and contains',
    'libogg and libvorbis (Xiph.Org Foundation, BSD-3-Clause, reproduced below) and miniaudio',
    '(David Reid, public domain / MIT-0).',
    '',
    (Get-Content -Raw (Join-Path $Root 'third_party/VERSIONS.md')),
    '',
    '## libogg',
    '',
    (Get-Content -Raw (Join-Path $Root 'third_party/libogg/COPYING')),
    '',
    '## libvorbis',
    '',
    (Get-Content -Raw (Join-Path $Root 'third_party/libvorbis/COPYING')),
    '',
    (Get-Content -Raw (Join-Path $Root 'third_party/steamaudio/THIRDPARTY.md'))
) -join [Environment]::NewLine
Set-Content -Path (Join-Path $modArtifact 'THIRD_PARTY_NOTICES.md') -Value $notices

# -Split (releases): the mod database takes files up to 40 MB, and Steam Audio's library alone is
# 15-19 MB compressed per platform. The main mod keeps the Windows libraries; the Linux and macOS
# ones move to a second mod, "spatialaudiounix", which the main mod looks in when it has none of
# its own for the platform (NativeLibraryResolver.FindNativeDirectory). Without -Split there is
# one zip with whatever was built, which is what a developer on any platform wants to install.
$packArtifact = Join-Path $Artifacts 'mod-unix-natives'
if (Test-Path $packArtifact) { Remove-Item -Recurse -Force $packArtifact }
if ($Split) {
    $packBuild = Join-Path $Root "src/SpatialAudioUnixNatives/bin/$Configuration/net10.0"
    New-Item -ItemType Directory -Force (Join-Path $packArtifact 'native') | Out-Null
    foreach ($file in 'SpatialAudioUnixNatives.dll', 'modinfo.json', 'modicon.png') {
        Copy-Item (Join-Path $packBuild $file) $packArtifact
    }
    Copy-Item (Join-Path $modArtifact 'THIRD_PARTY_NOTICES.md') $packArtifact
    $moved = @(Get-ChildItem (Join-Path $modArtifact 'native') -Directory | Where-Object Name -ne 'win-x64')
    if ($moved.Count -eq 0) { throw '-Split: there are no Linux or macOS libraries in artifacts/native to put in the native pack.' }
    foreach ($dir in $moved) { Move-Item $dir.FullName (Join-Path $packArtifact 'native') }
}

$limit = 40MB
$packages = @($modArtifact)
if ($Split) { $packages += $packArtifact }
foreach ($folder in $packages) {
    $modInfo = Get-Content -Raw (Join-Path $folder 'modinfo.json') | ConvertFrom-Json
    $zip = Join-Path $Artifacts "$($modInfo.modid)_$($modInfo.version).zip"
    if (Test-Path $zip) { Remove-Item -Force $zip }
    Compress-Archive -Path (Join-Path $folder '*') -DestinationPath $zip
    $size = (Get-Item $zip).Length
    Write-Host ("`nBuilt {0} ({1:N1} MB)" -f $zip, ($size / 1MB)) -ForegroundColor Green
    $platforms = (Get-ChildItem (Join-Path $folder 'native') -Directory -ErrorAction SilentlyContinue | ForEach-Object Name) -join ', '
    Write-Host "Native platforms in package: $platforms"
    if ($Split -and $size -gt $limit) { throw "$zip is over the mod database's 40 MB limit." }
}
