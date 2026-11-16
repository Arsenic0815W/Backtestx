#!/usr/bin/env python3
"""Pure-Python (standard library only, no numpy) reference for the BacktestX analytics.

For every scenario folder under the given directory (default: build/diff) it recomputes EVERY metric from the exported
files alone - equity.csv, trades.csv, fills.csv, optional benchmark.csv, and the config block of summary.json - and compares
the result with the metrics the C++ program wrote into summary.json.

    python tools/ref_metrics.py [DIR]

Agreement rules (the checker is meant to be hard to pass by accident):
  * the SET OF KEYS must match in both directions (a metric present on one side only is a failure);
  * integers, booleans, strings (dates) and nulls must match exactly, and an int may not be compared with a float;
  * floats must satisfy |a - b| <= REL_TOL * max(|a|, |b|) + ABS_FLOOR, with REL_TOL = 1e-9 (the project requirement) and
    ABS_FLOOR = 1e-12 only so that legitimately-near-zero values (a mean return, an alpha that is exactly 0) are not judged by
    a pure relative test;
  * it exits non-zero if DIR holds no scenario at all, so an empty or stale folder can never pass.

This file is written from the metric definitions in LEARNING.md / analytics.hpp, not by porting the C++ code.
Definitions in one place (n = number of equity records, E = equity in paise):
  r_i = E_i/E_{i-1} - 1;  years = (last date - first date).days / 365.25;  rf_d = rf / trading_days
  CAGR = (E_last/E_first)^(1/years) - 1;  vol = sd(r) * sqrt(td) with sample sd;  Sharpe = (mean(r) - rf_d)/sd(r) * sqrt(td)
  Sortino = (mean(r) - rf_d)/DD * sqrt(td), DD = sqrt(sum(min(r_i - rf_d, 0)^2)/len(r))
  drawdown_i = E_i / max(E_0..E_i) - 1; ...
"""
import csv
import datetime as dt
import json
import math
import os
import sys
from itertools import accumulate

REL_TOL = 1e-9
ABS_FLOOR = 1e-12
CAL_DAYS_PER_YEAR = 365.25


# ----------------------------------------------------------------------------- helpers
def d(s):
    return dt.date.fromisoformat(s)


def ds(x):
    return None if x is None else x.isoformat()


def mean(v):
    return sum(v) / len(v)


def sample_sd(v):
    m = mean(v)
    return math.sqrt(sum((x - m) ** 2 for x in v) / (len(v) - 1))


def read_csv(path):
    with open(path, newline="", encoding="utf-8") as f:
        return list(csv.DictReader(f))


# ----------------------------------------------------------------------------- equity
def equity_metrics(rows, cfg):
    td = cfg["trading_days_per_year"]
    rf_d = cfg["risk_free_annual"] / td
    n = len(rows)
    m = {k: None for k in (
        "start_date", "end_date", "start_equity_paise", "end_equity_paise", "total_return", "years", "cagr",
        "mean_daily_return", "daily_return_sd", "annualised_volatility", "sharpe", "sortino", "max_drawdown",
        "max_drawdown_peak_date", "max_drawdown_trough_date", "max_drawdown_recovery_date", "max_drawdown_recovered",
        "max_drawdown_duration_days", "longest_underwater_days", "calmar", "best_day", "best_day_date", "worst_day",
        "worst_day_date", "time_in_market", "avg_exposure")}
    m["n_records"] = n
    if n == 0:
        return m
    dates = [d(r["date"]) for r in rows]
    eq = [int(r["equity_paise"]) for r in rows]
    pos = [int(r["positions_value_paise"]) for r in rows]
    m["start_date"], m["end_date"] = ds(dates[0]), ds(dates[-1])
    m["start_equity_paise"], m["end_equity_paise"] = eq[0], eq[-1]
    if eq[0] > 0:
        m["total_return"] = eq[-1] / eq[0] - 1.0
    years = (dates[-1] - dates[0]).days / CAL_DAYS_PER_YEAR
    if years > 0:
        m["years"] = years
    if years > 0 and eq[0] > 0 and eq[-1] > 0:
        m["cagr"] = (eq[-1] / eq[0]) ** (1.0 / years) - 1.0

    m["time_in_market"] = sum(1 for p in pos if p != 0) / n
    live = [(abs(p) / e) for p, e in zip(pos, eq) if e > 0]
    if live:
        m["avg_exposure"] = sum(live) / len(live)

    if all(e > 0 for e in eq):
        run_max = list(accumulate(eq, max))
        dd = [e / pk - 1.0 for e, pk in zip(eq, run_max)]
        trough = min(range(n), key=lambda i: (dd[i], i))  # lowest drawdown, earliest on ties
        m["max_drawdown"] = dd[trough]
        if dd[trough] < 0:
            peak = eq.index(run_max[trough])              # first date at which that peak level was reached
            m["max_drawdown_peak_date"] = ds(dates[peak])
            m["max_drawdown_trough_date"] = ds(dates[trough])
            rec = next((j for j in range(trough + 1, n) if eq[j] >= eq[peak]), None)
            m["max_drawdown_recovered"] = rec is not None
            if rec is not None:
                m["max_drawdown_recovery_date"] = ds(dates[rec])
            m["max_drawdown_duration_days"] = ((dates[rec] if rec is not None else dates[-1]) - dates[peak]).days
        else:
            m["max_drawdown_duration_days"] = 0
        # Underwater spells, measured against the RUNNING maximum: a spell is a maximal run of records with equity strictly
        # below the running max. It starts at the date the running max was first reached (the peak) and ends at the first
        # later record back at/above that max (or at the last date if that never happens).
        longest = 0
        i = 0
        while i < n:
            if eq[i] >= run_max[i]:
                i += 1
                continue
            peak_date = dates[eq.index(run_max[i])]
            e = next((j for j in range(i + 1, n) if eq[j] >= run_max[i]), None)
            end = dates[e] if e is not None else dates[-1]
            longest = max(longest, (end - peak_date).days)
            i = e if e is not None else n
        m["longest_underwater_days"] = longest
        if m["cagr"] is not None and m["max_drawdown"] < 0:
            m["calmar"] = m["cagr"] / abs(m["max_drawdown"])

        if n >= 2:
            r = [eq[i] / eq[i - 1] - 1.0 for i in range(1, n)]
            mu = mean(r)
            m["mean_daily_return"] = mu
            hi = max(range(len(r)), key=lambda i: (r[i], -i))
            lo = min(range(len(r)), key=lambda i: (r[i], i))
            m["best_day"], m["best_day_date"] = r[hi], ds(dates[hi + 1])
            m["worst_day"], m["worst_day_date"] = r[lo], ds(dates[lo + 1])
            down = math.sqrt(sum(min(x - rf_d, 0.0) ** 2 for x in r) / len(r))
            if down > 0:
                m["sortino"] = (mu - rf_d) / down * math.sqrt(td)
            if len(r) >= 2:
                sd = sample_sd(r)
                m["daily_return_sd"] = sd
                m["annualised_volatility"] = sd * math.sqrt(td)
                if sd > 0:
                    m["sharpe"] = (mu - rf_d) / sd * math.sqrt(td)
    return m


# ----------------------------------------------------------------------------- trades / fills
def trade_metrics(trades, fills, eq_rows, em):
    m = {}
    nets = [int(t["net_pnl_paise"]) for t in trades]
    wins = [x for x in nets if x > 0]
    losses = [x for x in nets if x < 0]
    m["num_trades"] = len(nets)
    m["num_wins"], m["num_losses"] = len(wins), len(losses)
    m["num_breakeven"] = len(nets) - len(wins) - len(losses)
    m["win_rate"] = len(wins) / len(nets) if nets else None
    m["avg_win_paise"] = sum(wins) / len(wins) if wins else None
    m["avg_loss_paise"] = sum(losses) / len(losses) if losses else None
    m["profit_factor"] = (sum(wins) / abs(sum(losses))) if losses else None
    m["best_trade_paise"] = max(nets) if nets else None
    m["worst_trade_paise"] = min(nets) if nets else None
    m["avg_holding_days"] = (sum((d(t["exit_date"]) - d(t["entry_date"])).days for t in trades) / len(trades)) if trades else None
    m["avg_holding_bars"] = (sum(int(t["exit_day"]) - int(t["entry_day"]) for t in trades) / len(trades)) if trades else None

    m["num_fills"] = len(fills)
    last_pos = {}
    for f in fills:
        last_pos[f["symbol"]] = int(f["position_after"])
    m["num_open_positions"] = sum(1 for v in last_pos.values() if v != 0)
    traded = sum(int(f["notional_paise"]) for f in fills)
    fees = sum(int(f["fees_paise"]) for f in fills)
    m["traded_value_paise"] = traded
    m["total_fees_paise"] = fees
    m["average_equity_paise"] = m["turnover"] = m["turnover_annual"] = None
    m["net_pnl_paise"] = m["gross_pnl_paise"] = m["fees_pct_gross_pnl"] = m["fees_pct_initial_capital"] = None
    if eq_rows:
        eqs = [int(r["equity_paise"]) for r in eq_rows]
        avg = sum(eqs) / len(eqs)
        m["average_equity_paise"] = avg
        if avg > 0:
            m["turnover"] = traded / (2.0 * avg)
            if em["years"] is not None:
                m["turnover_annual"] = m["turnover"] / em["years"]
        net = eqs[-1] - eqs[0]
        m["net_pnl_paise"], m["gross_pnl_paise"] = net, net + fees
        if net + fees > 0:
            m["fees_pct_gross_pnl"] = fees / (net + fees)
        if eqs[0] > 0:
            m["fees_pct_initial_capital"] = fees / eqs[0]
    return m


# ----------------------------------------------------------------------------- benchmark
def benchmark_metrics(name, bench_rows, eq_rows, cfg):
    td = cfg["trading_days_per_year"]
    rf_d = cfg["risk_free_annual"] / td
    bvals = {d(r["date"]): float(r["value"]) for r in bench_rows}
    evals = {d(r["date"]): int(r["equity_paise"]) for r in eq_rows}
    common = sorted(set(bvals) & set(evals))
    nc = len(common)
    m = {"name": name, "n_common_dates": nc, "n_pairs": max(nc - 1, 0),
         "n_curve_dates_without_benchmark": len(evals) - nc, "n_benchmark_dates_not_in_curve": len(bvals) - nc}
    for k in ("first_common_date", "last_common_date", "strategy_return", "benchmark_return", "excess_return",
              "benchmark_cagr", "beta", "alpha_annual", "correlation"):
        m[k] = None
    if nc == 0:
        return m
    m["first_common_date"], m["last_common_date"] = ds(common[0]), ds(common[-1])
    if nc < 2 or any(evals[x] <= 0 for x in common):
        return m
    s0, s1 = evals[common[0]], evals[common[-1]]
    b0, b1 = bvals[common[0]], bvals[common[-1]]
    m["strategy_return"] = s1 / s0 - 1.0
    m["benchmark_return"] = b1 / b0 - 1.0
    m["excess_return"] = m["strategy_return"] - m["benchmark_return"]
    years = (common[-1] - common[0]).days / CAL_DAYS_PER_YEAR
    if years > 0 and s1 > 0:
        m["benchmark_cagr"] = (b1 / b0) ** (1.0 / years) - 1.0
    if nc - 1 < 3:
        return m
    rs = [evals[b] / evals[a] - 1.0 for a, b in zip(common, common[1:])]
    rb = [bvals[b] / bvals[a] - 1.0 for a, b in zip(common, common[1:])]
    ms, mb = mean(rs), mean(rb)
    cov = sum((x - ms) * (y - mb) for x, y in zip(rs, rb)) / (len(rs) - 1)
    var_b = sum((y - mb) ** 2 for y in rb) / (len(rb) - 1)
    if var_b > 0:
        beta = cov / var_b
        m["beta"] = beta
        m["alpha_annual"] = ((ms - rf_d) - beta * (mb - rf_d)) * td
        sd_s = sample_sd(rs)
        if sd_s > 0:
            m["correlation"] = cov / (sd_s * math.sqrt(var_b))
    return m


# ----------------------------------------------------------------------------- scenario driver
def compute(folder):
    """Recompute the metrics of one scenario folder from its exported files. Returns a summary-shaped dict."""
    with open(os.path.join(folder, "summary.json"), encoding="utf-8") as f:
        cfg = json.load(f)["config"]
    if cfg["calendar_days_per_year"] != CAL_DAYS_PER_YEAR:
        raise ValueError("calendar_days_per_year differs from the reference constant")
    eq_rows = read_csv(os.path.join(folder, "equity.csv"))
    trades = read_csv(os.path.join(folder, "trades.csv"))
    fills = read_csv(os.path.join(folder, "fills.csv"))
    em = equity_metrics(eq_rows, cfg)
    out = {"config": cfg, "equity": em, "trades": trade_metrics(trades, fills, eq_rows, em), "benchmark": None}
    bpath = os.path.join(folder, "benchmark.csv")
    if os.path.exists(bpath):
        out["benchmark"] = benchmark_metrics("", read_csv(bpath), eq_rows, cfg)  # name is taken from summary.json below
    return out


def compare_values(a, b):
    """Returns (ok, relative_difference_or_None)."""
    if a is None or b is None:
        return a is None and b is None, None
    if isinstance(a, (bool, str)) or isinstance(b, (bool, str)):
        return a == b and type(a) is type(b), None
    a_int, b_int = isinstance(a, int), isinstance(b, int)
    if a_int and b_int:
        return a == b, None
    if a_int != b_int:
        return False, None  # an int compared with a float: the output type drifted
    diff = abs(a - b)
    scale = max(abs(a), abs(b))
    return diff <= REL_TOL * scale + ABS_FLOOR, (diff / scale if scale > 0 else 0.0)


def compare_section(name, expected, actual, problems, stats):
    if expected is None or actual is None:
        if not (expected is None and actual is None):
            problems.append(f"{name}: one side is null, the other is not")
        return
    only_e = sorted(set(expected) - set(actual))
    only_a = sorted(set(actual) - set(expected))
    if only_e:
        problems.append(f"{name}: metrics only in summary.json (C++): {only_e}")
    if only_a:
        problems.append(f"{name}: metrics only in the Python reference: {only_a}")
    for k in sorted(set(expected) & set(actual)):
        ok, rel = compare_values(expected[k], actual[k])
        stats["compared"] += 1
        if rel is not None:
            stats["max_rel"] = max(stats["max_rel"], rel)
        if not ok:
            problems.append(f"{name}.{k}: C++ {expected[k]!r} vs Python {actual[k]!r}")


def check_folder(folder):
    """Returns (problems, stats)."""
    with open(os.path.join(folder, "summary.json"), encoding="utf-8") as f:
        summary = json.load(f)
    got = compute(folder)
    stats = {"compared": 0, "max_rel": 0.0}
    problems = []
    if got["benchmark"] is not None and summary.get("benchmark"):
        got["benchmark"]["name"] = summary["benchmark"].get("name")  # the name is a label, not computed
    elif (got["benchmark"] is None) != (summary.get("benchmark") is None):
        problems.append("benchmark.csv presence disagrees with summary.json['benchmark']")
    for sec in ("config", "equity", "trades", "benchmark"):
        compare_section(sec, summary.get(sec), got[sec], problems, stats)
    extra = sorted(set(summary) - {"config", "equity", "trades", "benchmark"})
    if extra:
        problems.append(f"unexpected top-level sections in summary.json: {extra}")
    return problems, stats


def main(argv):
    root = argv[1] if len(argv) > 1 else os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "build", "diff")
    if not os.path.isdir(root):
        print(f"ERROR: {root} does not exist (run the unit tests first so they export the scenarios)")
        return 1
    folders = sorted(os.path.join(root, x) for x in os.listdir(root)
                     if os.path.isfile(os.path.join(root, x, "summary.json")))
    if not folders:
        print(f"ERROR: no scenario folders (with summary.json) found in {root}")
        return 1
    print(f"tolerance: float |a-b| <= {REL_TOL:g} * max(|a|,|b|) + {ABS_FLOOR:g}; ints/dates/strings exact; key sets both ways")
    failed, total, worst = 0, 0, 0.0
    for fo in folders:
        try:
            problems, stats = check_folder(fo)
        except Exception as e:  # a crash is a failure, with the scenario named
            problems, stats = [f"crashed: {type(e).__name__}: {e}"], {"compared": 0, "max_rel": 0.0}
        total += stats["compared"]
        worst = max(worst, stats["max_rel"])
        status = "OK  " if not problems else "FAIL"
        print(f"{status} {os.path.basename(fo):34} metrics compared: {stats['compared']:3}  max relative difference: {stats['max_rel']:.2e}")
        for p in problems:
            print("      " + p)
        failed += bool(problems)
    print(f"{len(folders)} scenarios, {total} metric comparisons, worst relative difference {worst:.2e}, {failed} failing")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
