#!/usr/bin/env python3
"""How much does the benchmark choice change alpha? Same strategy, same window, two benchmarks, through the real backtestx.exe.

    python tools/analysis/benchmark_alpha_comparison.py [--symbol RELIANCE.NS] [--start 2012-01-02] [--data DIR]

Runs buy_and_hold on one stock (adjusted prices, allocation 0.95, null rows skipped) twice, once against ^NSEI (a price index) and once
against NIFTYBEES.NS (the default benchmark, with the known-bad bars from tools/analysis/known_bad_bars.json excluded), then prints
beta, annualised alpha, correlation, the benchmark's own CAGR and the difference between the two alphas. Outputs go to
build/analysis/ (not tracked). Needs build/backtestx.exe (run .\\build.ps1) and the real data files; skips cleanly without the data.
This is a measurement of the benchmark effect on ONE strategy and window, not evidence about the strategy.
"""
import argparse
import json
import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))  # needed under `python -I`
import _common as C  # noqa: E402


def run_once(exe, out_root, label, symbol, bench, start, excl, data_dir):
    cfg = {
        "name": f"alpha_cmp_{label}",
        "data": {"dir": data_dir.replace("\\", "/"), "files": [symbol], "null_rows": "skip", "exclude_dates": excl},
        "period": {"start": start},
        "capital": 1000000,
        "sizing": {"allocation": 0.95},
        "strategy": {"name": "buy_and_hold", "params": {"symbol": symbol}},
        "benchmark": bench,
    }
    os.makedirs(out_root, exist_ok=True)
    cfg_path = os.path.join(out_root, f"{label}.json")
    with open(cfg_path, "w", encoding="utf-8") as f:
        json.dump(cfg, f, indent=1)
    out = os.path.join(out_root, label)
    r = subprocess.run([exe, "run", "--config", cfg_path, "--out", out], capture_output=True, text=True)
    if r.returncode != 0:
        print(f"ERROR: backtestx run failed for {label} (exit {r.returncode}):\n{r.stderr}")
        sys.exit(1)
    with open(os.path.join(out, "summary.json"), encoding="utf-8") as f:
        return json.load(f)["benchmark"]


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--symbol", default="RELIANCE.NS")
    ap.add_argument("--start", default="2012-01-02")
    ap.add_argument("--data", default=C.DEFAULT_DATA)
    a = ap.parse_args()
    C.require_files(a.data, [a.symbol, "^NSEI", "NIFTYBEES.NS"])
    exe = os.path.join(C.REPO, "build", "backtestx.exe")
    if not os.path.exists(exe):
        print("ERROR: build/backtestx.exe not found; run .\\build.ps1 first")
        return 1
    out_root = os.path.join(C.REPO, "build", "analysis")
    bad = {s: [d.isoformat() for d in v] for s, v in C.known_bad_bars().items()}
    idx = run_once(exe, out_root, "vs_nsei", a.symbol, "^NSEI", a.start, {}, a.data)
    etf = run_once(exe, out_root, "vs_niftybees", a.symbol, "NIFTYBEES.NS", a.start, bad, a.data)

    print(f"{a.symbol} buy-and-hold, adjusted prices, window from {a.start}")
    print(f"{'benchmark':14} {'pairs':>6} {'beta':>7} {'alpha/yr':>9} {'corr':>7} {'bench CAGR':>11} {'excess return':>14}")
    for name, b in (("^NSEI", idx), ("NIFTYBEES.NS", etf)):
        print(f"{name:14} {b['n_pairs']:>6} {b['beta']:>7.3f} {C.pct(b['alpha_annual']):>9} {b['correlation']:>7.3f} "
              f"{C.pct(b['benchmark_cagr']):>11} {C.pct(b['excess_return']):>14}")
    print(f"alpha against the price index minus alpha against the ETF proxy: {(idx['alpha_annual'] - etf['alpha_annual']) * 100:+.2f} points/yr")
    print(f"benchmark CAGR, ETF minus index (same window): {(etf['benchmark_cagr'] - idx['benchmark_cagr']) * 100:+.2f} points/yr")
    return 0


if __name__ == "__main__":
    sys.exit(main())
