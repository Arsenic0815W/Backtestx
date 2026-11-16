<#
  Real-data consistency check (OPTIONAL; not part of build.ps1).

  Runs every built-in strategy through the real backtestx.exe on the real Yahoo files in data\ against the default benchmark
  (NIFTYBEES.NS, adjusted, with the four known-bad closes excluded) or, with -Benchmark '^NSEI', against the index, then pushes
  every run's exported files through tools\ref_metrics.py (the pure-Python recomputation of every metric).

  This is CONSISTENCY evidence only: it shows the engine, the exporters and the Python reference agree on real data, with
  holes, null rows and a late/odd benchmark. It makes no statement about strategy quality and prints no performance verdict.

  Usage:   .\tools\realdata_check.ps1 [-DataDir data] [-Benchmark NIFTYBEES.NS | ^NSEI]
  Needs:   build\backtestx.exe (run .\build.ps1 first), python, and the files from `python tools\fetch_data.py`.
  Results: build\realdata\out\<strategy>\  (equity.csv, trades.csv, fills.csv, benchmark.csv, summary.json, run.json)

  Exit status: 0 = checked and all metrics agree, OR nothing to check (a clear SKIPPED message when NO real data file exists);
               1 = real data is only PARTLY present, a run failed, or the Python reference disagrees with the C++.
#>
param(
  [string]$DataDir = (Join-Path (Split-Path $PSScriptRoot -Parent) 'data'),
  [ValidateSet('NIFTYBEES.NS', '^NSEI')][string]$Benchmark = 'NIFTYBEES.NS'
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Fail([string]$msg) {
  Write-Host "REAL-DATA CHECK FAILED: $msg" -ForegroundColor Red
  exit 1
}

$root  = Split-Path $PSScriptRoot -Parent
$stocks = 'RELIANCE.NS', 'TCS.NS', 'HDFCBANK.NS', 'INFY.NS'
$benchFile = if ($Benchmark -eq '^NSEI') { 'NSEI.csv' } else { "$Benchmark.csv" }    # '^' is dropped from file names
$required = @($stocks | ForEach-Object { "$_.csv" }) + $benchFile

# ---- are the real files there? none = SKIPPED (exit 0); some but not all = error ----
$present = @($required | Where-Object { Test-Path (Join-Path $DataDir $_) })
if ($present.Count -eq 0) {
  Write-Host "SKIPPED: none of the real data files were found in '$DataDir'." -ForegroundColor Yellow
  Write-Host "         Expected: $($required -join ', ')" -ForegroundColor Yellow
  Write-Host "         Download them with:  python tools\fetch_data.py   (real data is for your own research; it is not shipped)" -ForegroundColor Yellow
  exit 0
}
if ($present.Count -ne $required.Count) {
  $missing = @($required | Where-Object { $present -notcontains $_ })
  Fail "real data is only partly present in '$DataDir'. Missing: $($missing -join ', ')"
}

$exe = Join-Path $root 'build\backtestx.exe'
if (-not (Test-Path $exe)) { Fail "build\backtestx.exe not found; run .\build.ps1 first" }
if (-not (Get-Command python -ErrorAction SilentlyContinue)) { Fail "python not found on PATH" }

# ---- fresh output folder ----
$work = Join-Path $root 'build\realdata'
if (Test-Path $work) { Remove-Item -Recurse -Force $work }
$cfgDir = Join-Path $work 'configs'
$outDir = Join-Path $work 'out'
New-Item -ItemType Directory -Force $cfgDir | Out-Null
New-Item -ItemType Directory -Force $outDir | Out-Null

# Known-bad NIFTYBEES.NS closes (found by inspection; see data\README.md). The CSV is never edited: the run config drops them
# and records that it did. Not needed for ^NSEI.
$exclude = @{}
if ($Benchmark -eq 'NIFTYBEES.NS') {
    # single source of truth for the known-bad closes: tools/analysis/known_bad_bars.json (the detector in tools/analysis/niftybees_vs_index.py checks it)
    $known = Get-Content (Join-Path $PSScriptRoot 'analysis\known_bad_bars.json') -Raw | ConvertFrom-Json
    $exclude['NIFTYBEES.NS'] = @($known.'NIFTYBEES.NS')
}

$runs = [ordered]@{
  buy_and_hold       = @{ name = 'buy_and_hold';       params = @{ symbol = 'RELIANCE.NS' } }
  sma_crossover      = @{ name = 'sma_crossover';      params = @{ symbol = 'RELIANCE.NS'; fast = 20; slow = 50 } }
  rsi_mean_reversion = @{ name = 'rsi_mean_reversion'; params = @{ symbol = 'RELIANCE.NS' } }
  donchian_breakout  = @{ name = 'donchian_breakout';  params = @{ symbol = 'RELIANCE.NS' } }
  momentum_rotation  = @{ name = 'momentum_rotation';  params = @{ lookback = 126; top_k = 2 } }
}

Write-Host "real-data check: $($runs.Count) strategies on $($stocks -join ', ') vs $Benchmark (adjusted), window from 2012-01-02" -ForegroundColor Cyan
$dataDirFull = (Resolve-Path $DataDir).Path -replace '\\', '/'
foreach ($key in $runs.Keys) {
  $cfg = [ordered]@{
    name      = "real_$key"
    data      = [ordered]@{ dir = $dataDirFull; files = $stocks; null_rows = 'skip'; exclude_dates = $exclude }
    period    = @{ start = '2012-01-02' }       # 2010-2011 is warm-up history for every strategy
    capital   = 1000000
    sizing    = @{ allocation = 0.95 }
    strategy  = $runs[$key]
    benchmark = $Benchmark
  }
  $cfgPath = Join-Path $cfgDir "$key.json"
  $cfg | ConvertTo-Json -Depth 10 | Set-Content -Encoding ascii $cfgPath
  & $exe run --config $cfgPath --out (Join-Path $outDir $key) | Out-Null
  if ($LASTEXITCODE -ne 0) { Fail "backtestx run for '$key' (exit $LASTEXITCODE); config: $cfgPath" }
  Write-Host "  ran $key"
}

Write-Host "verifying every metric with tools\ref_metrics.py ..." -ForegroundColor Cyan
& python -I (Join-Path $PSScriptRoot 'ref_metrics.py') $outDir
if ($LASTEXITCODE -ne 0) { Fail "tools\ref_metrics.py: the Python reference disagrees with the C++ metrics (exit $LASTEXITCODE)" }

Write-Host "REAL-DATA CHECK OK: $($runs.Count) runs, outputs in $outDir (consistency only; not a performance claim)" -ForegroundColor Green
exit 0
