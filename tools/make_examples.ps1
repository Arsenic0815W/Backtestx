<#
  Regenerates the example reports in examples\ from the SYNTHETIC sample data (data\sample; no real prices are shipped):
    examples\sma_crossover\report.html         one single run  (configs\sma.json)
    examples\sma_sweep\report.html             a parameter sweep with an in-sample / out-of-sample split (configs\sma_sweep.json)
    examples\sma_sweep\best\report.html        the selected parameters' out-of-sample run (linked from the sweep report)
  Only the report files are copied; the full run folders stay in build\examples_a (they contain absolute paths in run.json).
  The reports are generated TWICE, from two different working directories, and the script fails unless the bytes are identical.
  Every generated folder is also checked by tools\check_report.py. Needs build\backtestx.exe (run .\build.ps1 first).
#>
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$exe = Join-Path $root 'build\backtestx.exe'
if (-not (Test-Path $exe)) { Write-Host "FAILED: $exe not found; run .\build.ps1 first" -ForegroundColor Red; exit 1 }

function Generate([string]$outRoot, [string]$workDir) {
  if (Test-Path $outRoot) { Remove-Item -Recurse -Force $outRoot }
  Push-Location $workDir
  try {
    & $exe run   --config (Join-Path $root 'configs\sma.json')       --out (Join-Path $outRoot 'sma_crossover') | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "backtestx run failed (exit $LASTEXITCODE)" }
    & $exe sweep --config (Join-Path $root 'configs\sma_sweep.json') --out (Join-Path $outRoot 'sma_sweep') --threads 2 | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "backtestx sweep failed (exit $LASTEXITCODE)" }
  } finally { Pop-Location }
}

$a = Join-Path $root 'build\examples_a'
$b = Join-Path $root 'build\examples_b'
Generate $a $root
Generate $b $env:TEMP

$reports = 'sma_crossover\report.html', 'sma_sweep\report.html', 'sma_sweep\best\report.html'
foreach ($r in $reports) {
  $ha = (Get-FileHash (Join-Path $a $r) -Algorithm SHA256).Hash
  $hb = (Get-FileHash (Join-Path $b $r) -Algorithm SHA256).Hash
  if ($ha -ne $hb) { Write-Host "FAILED: $r differs between two generations (not deterministic)" -ForegroundColor Red; exit 1 }
}
& python -I (Join-Path $PSScriptRoot 'check_report.py') (Join-Path $a 'sma_crossover') (Join-Path $a 'sma_sweep')
if ($LASTEXITCODE -ne 0) { Write-Host "FAILED: check_report.py" -ForegroundColor Red; exit 1 }

foreach ($r in $reports) {
  $dest = Join-Path $root "examples\$r"
  New-Item -ItemType Directory -Force (Split-Path -Parent $dest) | Out-Null
  Copy-Item (Join-Path $a $r) $dest -Force
  Write-Host ("wrote " + $dest + "  (" + (Get-Item $dest).Length + " bytes)")
}
Write-Host "examples OK: byte-identical across two generations from different working directories" -ForegroundColor Green
