#!/usr/bin/env python3
"""A real-data example report: an sma_crossover parameter sweep on RELIANCE.NS with an in-sample / out-of-sample split, against the
default benchmark NIFTYBEES.NS (known-bad closes excluded, tools/analysis/known_bad_bars.json), through the real backtestx.exe.

    python tools/analysis/real_sweep_example.py [--out examples/real_reliance_sweep] [--data DIR] [--markdown]

Writes the config and the whole sweep folder (sweep.csv, sweep_summary.json, best/, report.html) into --out, then prints what the
parameters selected IN-SAMPLE did OUT-OF-SAMPLE, read from the files the engine wrote. The output contains Yahoo-derived data, so it is
for your own research and must not be redistributed (keep it out of any published copy of the repo). Needs build/backtestx.exe and the
real files; skips cleanly without the data.
This shows how one parameter search behaved on one stock and one split. It is not evidence that the strategy works.
"""
import argparse
import json
import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))  # needed under `python -I`
import _common as C  # noqa: E402

NAME = "real_reliance_sma_sweep"


def make_config(data_dir):
    bad = {s: [d.isoformat() for d in v] for s, v in C.known_bad_bars().items()}
    return {
        "name": NAME,
        "data": {"dir": data_dir.replace("\\", "/"), "files": ["RELIANCE.NS"], "null_rows": "skip", "exclude_dates": bad},
        "period": {"start": "2012-01-02"},
        "capital": 1000000,
        "sizing": {"allocation": 0.95},
        "strategy": {"name": "sma_crossover", "params": {"symbol": "RELIANCE.NS", "fast": 20, "slow": 50}},
        "benchmark": "NIFTYBEES.NS",
        "sweep": {
            "grid": {"fast": [10, 20, 30, 50], "slow": [50, 100, 150, 200]},
            "metric": "sharpe",
            "min_trades": 10,
            "walk_forward": {"in_sample_fraction": 0.7},
        },
    }


def pct(x):
    return "n/a" if x is None else f"{x * 100:.2f}%"


def num(x):
    return "n/a" if x is None else f"{x:.3f}"


def print_markdown(out):
    """The IS-vs-OOS table of the README case study, read from the files a previous run wrote."""
    with open(os.path.join(out, "sweep_summary.json"), encoding="utf-8") as f:
        ss = json.load(f)
    with open(os.path.join(out, "best", "summary.json"), encoding="utf-8") as f:
        run = json.load(f)
    b, w = ss["best"], ss["windows"]
    print(f"Selected in-sample on {ss['selection_metric']}: fast {b['params']['fast']}, slow {b['params']['slow']} "
          f"(1 of {ss['grid']['n_combinations']} combinations; {ss['counts']['eligible_for_selection']} eligible).")
    print()
    print("| window | days | Sharpe | CAGR | total return | max drawdown | trades |")
    print("|---|---:|---:|---:|---:|---:|---:|")
    for label, key, win in (("in-sample", "in_sample", w["in_sample"]), ("out-of-sample", "out_of_sample", w["out_of_sample"])):
        m = b[key]
        print(f"| {label} {win['start']} to {win['end']} | {win['days']} | {num(m['sharpe'])} | {pct(m['cagr'])} | {pct(m['total_return'])} | "
              f"{pct(m['max_drawdown'])} | {m['num_trades']} |")
    rk = b["out_of_sample_rank"]
    bm = run.get("benchmark")
    print()
    print(f"- Out-of-sample {ss['selection_metric']} of the selected parameters ranks {rk['rank']} of {rk['of']} combinations.")
    if bm:
        print(f"- Over the same out-of-sample window {bm['name']} returned {pct(bm['benchmark_return'])} against the strategy's {pct(bm['strategy_return'])}.")
    for x in ss["warnings"]:
        print(f"- {x}")
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--out", default=os.path.join(C.REPO, "examples", "real_reliance_sweep"))
    ap.add_argument("--data", default=C.DEFAULT_DATA)
    ap.add_argument("--markdown", action="store_true",
                    help="do not run anything: print the README case-study table from an existing --out folder")
    a = ap.parse_args()
    if a.markdown:
        return print_markdown(a.out)
    C.require_files(a.data, ["RELIANCE.NS", "NIFTYBEES.NS"])
    exe = os.path.join(C.REPO, "build", "backtestx.exe")
    if not os.path.exists(exe):
        print("ERROR: build/backtestx.exe not found; run .\\build.ps1 first")
        return 1
    os.makedirs(a.out, exist_ok=True)
    cfg_path = os.path.join(a.out, "config.json")
    with open(cfg_path, "w", encoding="utf-8") as f:
        json.dump(make_config(os.path.abspath(a.data)), f, indent=1)
    r = subprocess.run([exe, "sweep", "--config", cfg_path, "--out", a.out], capture_output=True, text=True)
    sys.stdout.write(r.stdout)
    sys.stderr.write(r.stderr)
    if r.returncode != 0:
        print(f"ERROR: backtestx sweep exited {r.returncode}")
        return 1

    with open(os.path.join(a.out, "sweep_summary.json"), encoding="utf-8") as f:
        ss = json.load(f)
    with open(os.path.join(a.out, "best", "summary.json"), encoding="utf-8") as f:
        best_run = json.load(f)
    b, w = ss["best"], ss["windows"]
    print("\n== what the in-sample choice did out of sample ==")
    print(f"combinations {ss['grid']['n_combinations']}  (ran {ss['counts']['ok']}, invalid {ss['counts']['invalid']}, eligible {ss['counts']['eligible_for_selection']})")
    print(f"in-sample window {w['in_sample']['start']} .. {w['in_sample']['end']} ({w['in_sample']['days']} days); "
          f"out-of-sample {w['out_of_sample']['start']} .. {w['out_of_sample']['end']} ({w['out_of_sample']['days']} days)")
    print(f"selected on {ss['selection_metric']} in-sample: run {b['run_id']} {b['params']}")
    for label, m in (("in-sample ", b["in_sample"]), ("out-of-sample", b["out_of_sample"])):
        print(f"  {label:13} Sharpe {num(m['sharpe'])}  CAGR {pct(m['cagr'])}  total return {pct(m['total_return'])}  "
              f"max drawdown {pct(m['max_drawdown'])}  trades {m['num_trades']}")
    rk = b["out_of_sample_rank"]
    if rk:
        print(f"  its out-of-sample {ss['selection_metric']} ranks {rk['rank']} of {rk['of']} combinations")
    bm = best_run.get("benchmark")
    if bm:
        print(f"  out-of-sample run vs {bm['name']}: strategy return {pct(bm['strategy_return'])}, benchmark return {pct(bm['benchmark_return'])}, "
              f"excess {pct(bm['excess_return'])}, beta {num(bm['beta'])}")
    for wmsg in ss["warnings"]:
        print("  " + wmsg)
    print(f"\nreport: {os.path.join(a.out, 'report.html')}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
