#Requires -Version 7.0
<#
.SYNOPSIS
    Downloads pinned third-party dependencies listed in third_party/deps.json.
.DESCRIPTION
    Every download is verified against its SHA-256 before use; a mismatch aborts.
    A dependency whose stamp matches its pinned hash is skipped.
    Destinations are deleted before being replaced, so anything outside third_party/ is refused.
#>
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$Root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$ThirdParty = [IO.Path]::GetFullPath((Join-Path $Root 'third_party'))
$Manifest = Get-Content -Raw (Join-Path $ThirdParty 'deps.json') | ConvertFrom-Json
$Downloads = Join-Path $ThirdParty '.downloads'
New-Item -ItemType Directory -Force -Path $Downloads | Out-Null

function Assert-SafeDestination([string]$Path) {
    $full = [IO.Path]::GetFullPath($Path)
    $prefix = $ThirdParty.TrimEnd([IO.Path]::DirectorySeparatorChar) + [IO.Path]::DirectorySeparatorChar
    if (-not $full.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase) -or $full.Length -le $prefix.Length) {
        throw "Refusing to write outside third_party/: '$full'"
    }
    return $full
}

foreach ($dep in $Manifest.dependencies) {
    if ($dep.kind -notin @('zip', 'file')) { throw "Unknown kind '$($dep.kind)' for $($dep.name)" }
    $dest = Assert-SafeDestination (Join-Path $Root $dep.destination)
    $stamp = Join-Path $Downloads "$($dep.name).sha256"
    if ((Test-Path $stamp) -and ((Get-Content -Raw $stamp).Trim() -eq $dep.sha256) -and (Test-Path $dest)) {
        Write-Host "[deps] $($dep.name): up to date"
        continue
    }

    $file = Join-Path $Downloads "$($dep.name).download"
    Write-Host "[deps] $($dep.name): downloading $($dep.url)"
    Invoke-WebRequest -Uri $dep.url -OutFile $file -MaximumRetryCount 3
    $actual = (Get-FileHash -Algorithm SHA256 $file).Hash.ToLowerInvariant()
    if ($actual -ne $dep.sha256) {
        Remove-Item $file -Force
        throw "$($dep.name) hash mismatch (expected $($dep.sha256), got $actual)"
    }

    if (Test-Path $dest) { Remove-Item -Recurse -Force $dest }
    New-Item -ItemType Directory -Force -Path (Split-Path $dest) | Out-Null
    if ($dep.kind -eq 'zip') {
        $tmp = Join-Path $Downloads "$($dep.name).extract"
        if (Test-Path $tmp) { Remove-Item -Recurse -Force $tmp }
        Expand-Archive -Path $file -DestinationPath $tmp
        Move-Item (Join-Path $tmp $dep.archiveRoot) $dest
        Remove-Item -Recurse -Force $tmp
    }
    else {
        Copy-Item $file $dest
    }
    Remove-Item $file -Force
    Set-Content -Path $stamp -Value $dep.sha256 -NoNewline
    Write-Host "[deps] $($dep.name): ok"
}
