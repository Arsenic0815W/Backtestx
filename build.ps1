<#
  BacktestX build script (Windows, portable MinGW toolchain in D:\Tools).
  Configures, builds Release, runs the unit tests, the Python differential metrics check and the Python tool tests.
  Exits non-zero on ANY failure.
  Usage:  .\build.ps1            (Release build + tests)
          .\build.ps1 -Clean     (wipe build\ first)
#>
param([switch]$Clean)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Fail([string]$msg) {
  Write-Host "BUILD FAILED: $msg" -ForegroundColor Red
  exit 1
}

# Process-local only; the system/user PATH is never modified.
$env:PATH = "D:\Tools\w64devkit\bin;D:\Tools\cmake\bin;$env:PATH"

foreach ($tool in 'g++', 'make', 'cmake') {
  if (-not (Get-Command $tool -ErrorAction SilentlyContinue)) { Fail "$tool not found on PATH" }
}

$root  = $PSScriptRoot
$build = Join-Path $root 'build'
if ($Clean -and (Test-Path $build)) { Remove-Item -Recurse -Force $build }

Write-Host "== configure ==" -ForegroundColor Cyan
& cmake -S $root -B $build -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release
if ($LASTEXITCODE -ne 0) { Fail "cmake configure (exit $LASTEXITCODE)" }

Write-Host "== build (Release) ==" -ForegroundColor Cyan
& cmake --build $build -j
if ($LASTEXITCODE -ne 0) { Fail "cmake build (exit $LASTEXITCODE)" }

Write-Host "== tests ==" -ForegroundColor Cyan
$tests = Join-Path $build 'bt_tests.exe'
if (-not (Test-Path $tests)) { Fail "bt_tests.exe was not produced" }
# The unit tests export diverse finished runs to build\diff; delete any old export first so a stale or empty folder
# can never satisfy the differential check below.
$diff = Join-Path $build 'diff'
if (Test-Path $diff) { Remove-Item -Recurse -Force $diff }
& $tests
if ($LASTEXITCODE -ne 0) { Fail "bt_tests (exit $LASTEXITCODE)" }

Write-Host "== python: differential metrics check (C++ vs pure-Python recomputation) ==" -ForegroundColor Cyan
if (-not (Get-Command python -ErrorAction SilentlyContinue)) { Fail "python not found on PATH (needed for tools/)" }
$tools = Join-Path $root 'tools'
& python -I (Join-Path $tools 'ref_metrics.py') $diff
if ($LASTEXITCODE -ne 0) { Fail "tools/ref_metrics.py (exit $LASTEXITCODE): C++ metrics and the Python reference disagree" }

Write-Host "== CLI smoke: backtestx run + sweep on the shipped sample configs, verified by the Python reference ==" -ForegroundColor Cyan
$exe   = Join-Path $build 'backtestx.exe'
$cfgs  = Join-Path $root 'configs'
$smoke = Join-Path $build 'smoke'
if (Test-Path $smoke) { Remove-Item -Recurse -Force $smoke }
foreach ($c in 'sma', 'momentum') {
  & $exe run --config (Join-Path $cfgs "$c.json") --out (Join-Path $smoke $c)
  if ($LASTEXITCODE -ne 0) { Fail "backtestx run --config configs/$c.json (exit $LASTEXITCODE)" }
}
& $exe sweep --config (Join-Path $cfgs 'sma_sweep.json') --out (Join-Path $smoke 'sweep')
if ($LASTEXITCODE -ne 0) { Fail "backtestx sweep --config configs/sma_sweep.json (exit $LASTEXITCODE)" }
& python -I (Join-Path $tools 'ref_metrics.py') $smoke
if ($LASTEXITCODE -ne 0) { Fail "tools/ref_metrics.py on the CLI run outputs (exit $LASTEXITCODE)" }
& python -I (Join-Path $tools 'ref_metrics.py') (Join-Path $smoke 'sweep')
if ($LASTEXITCODE -ne 0) { Fail "tools/ref_metrics.py on the sweep's best/ run (exit $LASTEXITCODE)" }

Write-Host "== HTML reports ==" -ForegroundColor Cyan
# report.html was written by the CLI smoke runs above; check it independently against the files next to it.
foreach ($d in 'sma', 'momentum', 'sweep') {
  if (-not (Test-Path (Join-Path $smoke "$d\report.html"))) { Fail "the CLI did not write build\smoke\$d\report.html" }
}
& python -I (Join-Path $tools 'check_report.py') (Join-Path $smoke 'sma') (Join-Path $smoke 'momentum') (Join-Path $smoke 'sweep')
if ($LASTEXITCODE -ne 0) { Fail "tools/check_report.py (exit $LASTEXITCODE)" }
if (Get-Command node -ErrorAction SilentlyContinue) {
  & node (Join-Path $tools 'test_report_js.js')
  if ($LASTEXITCODE -ne 0) { Fail "tools/test_report_js.js (exit $LASTEXITCODE)" }
} else { Write-Host "SKIPPED: node not found; report.js formatting tests not run" -ForegroundColor Yellow }
# Headless Edge/Chrome: the page must finish drawing without an error (skips cleanly when no browser is installed).
& python -I (Join-Path $tools 'browser_check.py') (Join-Path $smoke 'sma\report.html') (Join-Path $smoke 'sweep\report.html') (Join-Path $smoke 'sweep\best\report.html') (Join-Path $smoke 'momentum\report.html')
if ($LASTEXITCODE -ne 0) { Fail "tools/browser_check.py (exit $LASTEXITCODE)" }

Write-Host "== bench smoke: bt_bench --quick (runs every workload, checks results agree; asserts nothing about speed) ==" -ForegroundColor Cyan
& (Join-Path $build 'bt_bench.exe') --quick | Out-Null
if ($LASTEXITCODE -ne 0) { Fail "bt_bench --quick (exit $LASTEXITCODE)" }

Write-Host "== python tool tests ==" -ForegroundColor Cyan
foreach ($t in 'test_fetch_data.py', 'test_ref_metrics.py', 'test_analysis.py') {
  & python -I (Join-Path $tools $t)
  if ($LASTEXITCODE -ne 0) { Fail "tools/$t (exit $LASTEXITCODE)" }
}

Write-Host "BUILD OK" -ForegroundColor Green
exit 0
