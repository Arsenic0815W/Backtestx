#!/usr/bin/env python3
"""How good is NIFTYBEES.NS (the default benchmark) as a total-return proxy for the Nifty 50, from the Yahoo files?

    python tools/analysis/niftybees_vs_index.py [--data DIR] [--threshold 0.08] [--json]

It reports, from data/NIFTYBEES.NS.csv and data/NSEI.csv:
  1. file facts: rows, null rows, whether Adj Close differs from Close at all;
  2. SUSPECT CLOSES found by a detector (not by eye): the ETF/index price ratio on common dates is compared with the median of
     the ratios of the three bars either side; a bar whose ratio is more than THRESHOLD (default 8 %) away is a suspect. Dividing by
     the index cancels real market moves, so only ETF-specific anomalies remain. The detected set is compared with
     tools/analysis/known_bad_bars.json (the list the run configs exclude);
  3. the proxy statistics with ALL bars and with the known-bad bars EXCLUDED: CAGR of each and the gap, annualised tracking error
     (sample sd of the daily return difference between consecutive common dates x sqrt(252)), daily-return correlation, largest daily
     return of each, and how many days still differ by more than 3 percentage points.

Whether a positive CAGR gap is dividends is NOT established by this script (non-synchronous closing prices could also contribute).
The ETF's expense ratio is not in the data and is not estimated here. Skips cleanly without the real files.
"""
import argparse
import json
import statistics
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))  # needed under `python -I`
import _common as C  # noqa: E402


def suspects(etf, idx, threshold, half_window=3):
    common = sorted(set(etf) & set(idx))
    ratio = {d: etf[d].close / idx[d].close for d in common}
    out = []
    # Three bars each side (6 neighbours): the median stays correct even when TWO consecutive bars are bad. (An earlier version
    # used two bars each side; with a two-bar glitch the good bars next to it were contaminated and falsely flagged.)
    for k in range(half_window, len(common) - half_window):
        neigh = [ratio[common[j]] for j in list(range(k - half_window, k)) + list(range(k + 1, k + half_window + 1))]
        dev = ratio[common[k]] / statistics.median(neigh) - 1.0
        if abs(dev) > threshold:
            out.append((common[k], dev))
    return out


def proxy_stats(etf, idx, exclude):
    common = sorted((set(etf) & set(idx)) - set(exclude))
    e = [etf[d].adj for d in common]
    i = [idx[d].adj for d in common]
    days = (common[-1] - common[0]).days
    re = [e[k] / e[k - 1] - 1 for k in range(1, len(e))]
    ri = [i[k] / i[k - 1] - 1 for k in range(1, len(i))]
    diff = [a - b for a, b in zip(re, ri)]
    far = [(common[k + 1], re[k], ri[k]) for k in range(len(re)) if abs(diff[k]) > 0.03]
    return {
        "common_dates": len(common),
        "etf_cagr": C.cagr(e[0], e[-1], days),
        "index_cagr": C.cagr(i[0], i[-1], days),
        "cagr_gap": C.cagr(e[0], e[-1], days) - C.cagr(i[0], i[-1], days),
        "tracking_error_annualised": C.sample_sd(diff) * 252 ** 0.5,
        "daily_return_correlation": C.correlation(re, ri),
        "largest_abs_return_etf": max(abs(x) for x in re),
        "largest_abs_return_index": max(abs(x) for x in ri),
        "days_differing_by_over_3_points": len(far),
        "examples_of_those_days": [(d.isoformat(), round(a * 100, 2), round(b * 100, 2)) for d, a, b in far[:5]],
    }


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--data", default=C.DEFAULT_DATA)
    ap.add_argument("--threshold", type=float, default=0.08)
    ap.add_argument("--json", action="store_true")
    a = ap.parse_args()
    paths = C.require_files(a.data, ["NIFTYBEES.NS", "^NSEI"])
    etf_bars, etf_nulls = C.load_csv(paths["NIFTYBEES.NS"])
    idx_bars, _ = C.load_csv(paths["^NSEI"])
    etf = {b.date: b for b in etf_bars}
    idx = {b.date: b for b in idx_bars}
    known = set(C.known_bad_bars().get("NIFTYBEES.NS", []))
    found = suspects(etf, idx, a.threshold)
    found_dates = {d for d, _ in found}

    result = {
        "file": {
            "rows": len(etf_bars) + len(etf_nulls),
            "null_rows": len(etf_nulls),
            "null_rows_on_weekdays": [d.isoformat() for d in etf_nulls if d.weekday() < 5],
            "adj_close_equals_close_everywhere": all(b.adj == b.close for b in etf_bars),
            "zero_volume_bars": sum(1 for b in etf_bars if b.volume == 0),
        },
        "detector": {"threshold": a.threshold, "suspects": [(d.isoformat(), round(dev * 100, 1)) for d, dev in found],
                     "equals_known_bad_list": found_dates == known,
                     "known_not_detected": sorted(d.isoformat() for d in known - found_dates),
                     "detected_not_known": sorted(d.isoformat() for d in found_dates - known)},
        "all_bars": proxy_stats(etf, idx, []),
        "known_bad_excluded": proxy_stats(etf, idx, known),
    }
    if a.json:
        print(json.dumps(result, indent=1))
        return 0
    f = result["file"]
    print(f"NIFTYBEES.NS: {f['rows']} rows, {f['null_rows']} null rows (weekdays: {', '.join(f['null_rows_on_weekdays']) or 'none'}), "
          f"{f['zero_volume_bars']} zero-volume bars; Adj Close == Close on every row: {f['adj_close_equals_close_everywhere']}")
    d = result["detector"]
    print(f"== suspect closes (ETF/index ratio vs the median of the 6 surrounding ratios, threshold {a.threshold:.0%}) ==")
    for dte, dev in d["suspects"]:
        print(f"  {dte}  ratio deviates {dev:+.1f}%")
    print(f"  detected set equals tools/analysis/known_bad_bars.json: {d['equals_known_bad_list']}"
          + ("" if d["equals_known_bad_list"] else f"  (known but not detected: {d['known_not_detected']}; detected but not known: {d['detected_not_known']})"))
    for label, key in (("ALL bars", "all_bars"), ("known-bad bars EXCLUDED", "known_bad_excluded")):
        s = result[key]
        print(f"== proxy statistics, {label} ({s['common_dates']} common dates) ==")
        print(f"  CAGR  ETF {C.pct(s['etf_cagr'], 3)}   index {C.pct(s['index_cagr'], 3)}   gap {s['cagr_gap'] * 100:+.3f} points/yr")
        print(f"  annualised tracking error {C.pct(s['tracking_error_annualised'])}   daily-return correlation {s['daily_return_correlation']:.4f}")
        print(f"  largest |daily return|  ETF {C.pct(s['largest_abs_return_etf'])}   index {C.pct(s['largest_abs_return_index'])}")
        print(f"  days where ETF and index returns differ by more than 3 points: {s['days_differing_by_over_3_points']}  {s['examples_of_those_days']}")
    print("note: a positive CAGR gap is consistent with accumulated income, but non-synchronous closes could also contribute; "
          "the expense ratio is not in the data.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
