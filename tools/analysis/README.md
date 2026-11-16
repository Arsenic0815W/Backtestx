# tools/analysis/

**Standing rule: any number or finding quoted in a report, in `LEARNING.md` or in the README must be reproducible by a script
in this folder (or by a test). Scratch scripts are for exploring; they never source a quoted figure.**

These scripts measure facts about the real Yahoo files in `data/` (not shipped; see `data/README.md`). Each one prints plain text,
accepts `--data DIR`, and **skips cleanly** (prints `SKIPPED`, exit 0) when none of its files are present. If only some are present it
fails loudly (exit 1) and names the missing file. None of them says anything about strategy performance.

| script | answers | quoted in |
|---|---|---|
| `data_quality.py` | rows, null rows, zero-volume bars, adjustment steps, largest daily moves, five-file calendar alignment | data/README.md, LEARNING.md (Phase 1, Phase 5 guard calibration) |
| `niftybees_vs_index.py` | is NIFTYBEES.NS a usable total-return proxy? bad-close detector, CAGR gap, tracking error, correlation | README, LEARNING.md (Phase 4 review, Phase 5) |
| `raw_vs_adjusted_signals.py` | why raw and adjusted SMA-crossover runs differ in trade count | LEARNING.md (Phase 3 review) |
| `benchmark_alpha_comparison.py` | alpha of one buy-and-hold against ^NSEI vs NIFTYBEES (drives `build/backtestx.exe`) | LEARNING.md (Phase 5) |
| `known_bad_bars.json` | the closes the run configs exclude; `realdata_check.ps1` reads it and the detector must reproduce it | |

`python -I tools/analysis/run_all.py` runs all four in order. `python -I tools/test_analysis.py` tests the logic on synthetic data
and runs inside `build.ps1`.

## Numbers that are NOT reproducible from this folder

Honest list (see LEARNING.md "Provenance of cited numbers"): the Phase 3 "before the fix" SMA/Bollinger error figures came from a
scratch driver run against the pre-fix code and cannot be regenerated; the post-fix behaviour is pinned by unit tests.
