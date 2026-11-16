<#
  Phase 7: runs bt_bench three times (separate processes), captures the machine and toolchain, and generates docs\BENCHMARKS.md.
    .\tools\run_benchmarks.ps1            (needs build\bt_bench.exe; run .\build.ps1 first)
  Leave the machine idle while it runs (no builds, no mutation runs). It takes about a minute.
  Raw outputs: docs\bench_runs\env.json, run1.json .. run3.json. The tables in docs\BENCHMARKS.md are generated from them by
  tools\bench_report.py; the hand-written discussion lives in docs\benchmarks_notes.md and is appended.
#>
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$env:PATH = "D:\Tools\w64devkit\bin;D:\Tools\cmake\bin;$env:PATH"
$exe = Join-Path $root 'build\bt_bench.exe'
if (-not (Test-Path $exe)) { Write-Host "FAILED: $exe not found; run .\build.ps1 first" -ForegroundColor Red; exit 1 }
$raw = Join-Path $root 'docs\bench_runs'
if (Test-Path $raw) { Remove-Item -Recurse -Force $raw }
New-Item -ItemType Directory -Force $raw | Out-Null

# ---- environment: read from the machine and the build, not from memory ----
$cpu = Get-CimInstance Win32_Processor | Select-Object -First 1
$cs  = Get-CimInstance Win32_ComputerSystem
$os  = Get-CimInstance Win32_OperatingSystem
$gpp = (& g++ --version | Select-Object -First 1)
$cache = Get-Content (Join-Path $root 'build\CMakeCache.txt')
function CacheVal([string]$k) { $l = $cache | Where-Object { $_ -like "${k}:*" } | Select-Object -First 1; if ($l) { ($l -split '=', 2)[1] } else { '(not set)' } }
$battery = Get-CimInstance Win32_Battery -ErrorAction SilentlyContinue | Select-Object -First 1
$power = if ($battery) { if ($battery.BatteryStatus -eq 2) { "laptop on AC power (battery $($battery.EstimatedChargeRemaining)%)" } else { "laptop on BATTERY ($($battery.EstimatedChargeRemaining)%, status $($battery.BatteryStatus))" } } else { 'no battery reported (desktop or unknown)' }
$scheme = (powercfg /getactivescheme) -replace '^.*:\s*', ''
$hw = [int]$cpu.NumberOfLogicalProcessors
$asc = @(1, 2, 4, $hw) | Sort-Object -Unique | Where-Object { $_ -le $hw }
$desc = @($asc | Sort-Object -Descending)
$rot = @($asc[2..($asc.Count - 1)] + $asc[0..1])
if ($asc.Count -lt 3) { $rot = @($desc) }
$orders = @(($asc -join ','), ($desc -join ','), ($rot -join ','))

$env = [ordered]@{
  date                    = (Get-Date -Format 'yyyy-MM-dd HH:mm')
  cpu                     = $cpu.Name.Trim()
  physical_cores          = $cpu.NumberOfCores
  logical_processors      = $cpu.NumberOfLogicalProcessors
  cpu_max_clock_mhz       = $cpu.MaxClockSpeed
  ram_gb                  = [math]::Round($cs.TotalPhysicalMemory / 1GB, 1)
  os                      = "$($os.Caption) build $($os.BuildNumber)"
  compiler                = $gpp
  cmake_build_type        = (CacheVal 'CMAKE_BUILD_TYPE')
  cxx_flags               = ((CacheVal 'CMAKE_CXX_FLAGS') + ' ' + (CacheVal 'CMAKE_CXX_FLAGS_RELEASE')).Trim()
  extra_flags_from_cmake  = '-Wall -Wextra -Werror (warnings only); C++17; no -march=native; no LTO'
  power                   = "$power; active plan: $scheme"
  thread_orders           = $orders
}
($env | ConvertTo-Json -Depth 4) | Set-Content (Join-Path $raw 'env.json') -Encoding utf8

# ---- three separate processes, thread-count order rotated ----
for ($i = 0; $i -lt 3; $i++) {
  Write-Host ("run {0}: thread order {1}" -f ($i + 1), $orders[$i]) -ForegroundColor Cyan
  $json = & $exe --json --threads $orders[$i]
  if ($LASTEXITCODE -ne 0) { Write-Host "FAILED: bt_bench exited $LASTEXITCODE" -ForegroundColor Red; exit 1 }
  ($json -join "`n") | Set-Content (Join-Path $raw ("run{0}.json" -f ($i + 1))) -Encoding utf8
}

& python -I (Join-Path $PSScriptRoot 'bench_report.py') $raw (Join-Path $root 'docs\BENCHMARKS.md') (Join-Path $root 'docs\benchmarks_notes.md') (Join-Path $root 'docs\bench_battery_baseline.json')
if ($LASTEXITCODE -ne 0) { Write-Host "FAILED: bench_report.py" -ForegroundColor Red; exit 1 }
