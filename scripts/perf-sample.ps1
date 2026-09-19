#Requires -Version 7.0
<#
.SYNOPSIS
    Samples the running Vintage Story process from outside: CPU, memory, threads. For the
    mod-against-vanilla comparison (docs/investigations/performance.md): run it once with the
    mod disabled and once enabled, standing at the same spot doing the same thing.
.PARAMETER Seconds
    How long to sample (default 60).
.PARAMETER Label
    A name for the run, used in the summary and the CSV's file name (default: a timestamp).
.PARAMETER OutDir
    Where the per-second CSV goes (default artifacts/perf).
.EXAMPLE
    pwsh ./scripts/perf-sample.ps1 -Seconds 60 -Label vanilla-village
    pwsh ./scripts/perf-sample.ps1 -Seconds 60 -Label mod-village
#>
param(
    [int]$Seconds = 60,
    [string]$Label = (Get-Date -Format 'yyyyMMdd-HHmmss'),
    [string]$OutDir = (Join-Path $PSScriptRoot '..' 'artifacts' 'perf')
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$game = Get-Process Vintagestory -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not $game) {
    throw 'Vintage Story is not running.'
}

New-Item -ItemType Directory -Force $OutDir | Out-Null
$csv = Join-Path $OutDir "$Label.csv"
$cores = [Environment]::ProcessorCount
$rows = [System.Collections.Generic.List[object]]::new()

$game.Refresh()
$lastCpu = $game.TotalProcessorTime
$lastTime = Get-Date
Write-Host "Sampling Vintagestory (pid $($game.Id)) for $Seconds s as '$Label'..."
for ($i = 0; $i -lt $Seconds; $i++) {
    Start-Sleep -Seconds 1
    $game.Refresh()
    if ($game.HasExited) { Write-Warning 'The game exited.'; break }
    $now = Get-Date
    $cpu = $game.TotalProcessorTime
    $wall = ($now - $lastTime).TotalSeconds
    $coresUsed = ($cpu - $lastCpu).TotalSeconds / $wall
    $rows.Add([pscustomobject]@{
        Time        = $now.ToString('HH:mm:ss')
        Cores       = [math]::Round($coresUsed, 3)
        CpuPercent  = [math]::Round(100 * $coresUsed / $cores, 1)
        WorkingSetMB = [math]::Round($game.WorkingSet64 / 1MB, 0)
        PrivateMB   = [math]::Round($game.PrivateMemorySize64 / 1MB, 0)
        Threads     = $game.Threads.Count
        Handles     = $game.HandleCount
    })
    $lastCpu = $cpu
    $lastTime = $now
}

$rows | Export-Csv -NoTypeInformation $csv
$stat = { param($name) $v = $rows | Measure-Object -Property $name -Average -Maximum -Minimum; [pscustomobject]@{ Avg = [math]::Round($v.Average, 2); Min = $v.Minimum; Max = $v.Maximum } }
$c = & $stat 'Cores'; $w = & $stat 'WorkingSetMB'; $p = & $stat 'PrivateMB'; $t = & $stat 'Threads'
Write-Host ''
Write-Host "$Label ($($rows.Count) s, $cores logical cores):"
Write-Host ("  CPU         avg {0} cores ({1} %), min {2}, max {3}" -f $c.Avg, [math]::Round(100 * $c.Avg / $cores, 1), $c.Min, $c.Max)
Write-Host ("  working set avg {0} MB, min {1}, max {2}" -f $w.Avg, $w.Min, $w.Max)
Write-Host ("  private     avg {0} MB, min {1}, max {2}" -f $p.Avg, $p.Min, $p.Max)
Write-Host ("  threads     avg {0}, min {1}, max {2}" -f $t.Avg, $t.Min, $t.Max)
Write-Host "  per-second rows: $csv"
