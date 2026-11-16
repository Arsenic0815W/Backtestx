#!/usr/bin/env python3
"""Facts about the real Yahoo files in data/: rows, null rows, adjustment steps, zero-volume rows, the largest daily moves
(which calibrate the implausible-move guard), and how the five-file calendar aligns.

    python tools/analysis/data_quality.py [--data DIR] [--json]

Source of the figures quoted in data/README.md and LEARNING.md (Phase 1 real-data findings, Phase 5 guard calibration).
Skips cleanly when the real files are absent. Makes no statement about strategy performance.
"""
import argparse
import datetime as dt
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))  # needed under `python -I`
import _common as C  # noqa: E402

SYMBOLS = ["RELIANCE.NS", "TCS.NS", "HDFCBANK.NS", "INFY.NS", "^NSEI", "NIFTYBEES.NS"]
FIVE = ["RELIANCE.NS", "TCS.NS", "HDFCBANK.NS", "INFY.NS", "^NSEI"]   # the files of the original five-file alignment


def file_facts(symbol, path, exclude=()):
    bars, nulls = C.load_csv(path)
    weekdays = [d for d in nulls if d.weekday() < 5]
    ratio = [b.adj / b.close for b in bars]
    steps = [bars[i].date for i in range(1, len(bars)) if abs(ratio[i] / ratio[i - 1] - 1) > 1e-5]
    moves = sorted(((abs(bars[i].close / bars[i - 1].close - 1), bars[i].date) for i in range(1, len(bars))), reverse=True)
    kept = [b for b in bars if b.date not in set(exclude)]
    moves_kept = sorted(((abs(kept[i].close / kept[i - 1].close - 1), kept[i].date) for i in range(1, len(kept))), reverse=True)
    return {
        "symbol": symbol,
        "rows": len(bars) + len(nulls),
        "bars": len(bars),
        "first": bars[0].date.isoformat(),
        "last": bars[-1].date.isoformat(),
        "null_rows": len(nulls),
        "null_rows_on_weekends": len(nulls) - len(weekdays),
        "null_rows_on_weekdays": len(weekdays),
        "null_weekday_dates": [d.isoformat() for d in weekdays],
        "adj_equals_close_everywhere": all(b.adj == b.close for b in bars),
        "adjustment_steps": len(steps),
        "first_adj_over_close": round(ratio[0], 4),
        "zero_volume_bars": sum(1 for b in bars if b.volume == 0),
        "largest_moves": [(round(m * 100, 1), d.isoformat()) for m, d in moves[:3]],
        "largest_moves_after_exclusions": [(round(m * 100, 1), d.isoformat()) for m, d in moves_kept[:3]],
    }


def alignment(paths):
    dates = {s: {b.date for b in C.load_csv(paths[s])[0]} for s in FIVE}
    union = set().union(*dates.values())
    stocks = [s for s in FIVE if s != "^NSEI"]
    all_five = [d for d in union if all(d in dates[s] for s in FIVE)]
    stock_no_index = [d for d in union if d not in dates["^NSEI"] and any(d in dates[s] for s in stocks)]
    index_no_stock = [d for d in union if d in dates["^NSEI"] and not any(d in dates[s] for s in stocks)]
    return {
        "files": FIVE,
        "union_calendar_days": len(union),
        "days_with_all_five": len(all_five),
        "days_with_a_stock_but_no_index_bar": len(stock_no_index),
        "days_with_the_index_but_no_stock_bar": len(index_no_stock),
        "weekend_days_in_calendar": sum(1 for d in union if d.weekday() >= 5),
        "stock_but_no_index_dates": sorted(d.isoformat() for d in stock_no_index),
    }


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--data", default=C.DEFAULT_DATA)
    ap.add_argument("--json", action="store_true")
    a = ap.parse_args()
    paths = C.require_files(a.data, SYMBOLS)
    bad = {s: C.known_bad_bars().get(s, []) for s in SYMBOLS}
    facts = [file_facts(s, paths[s], bad[s]) for s in SYMBOLS]
    align = alignment(paths)
    if a.json:
        print(json.dumps({"files": facts, "alignment": align}, indent=1))
        return 0
    print("== per-file facts ==")
    for f in facts:
        print(f"{f['symbol']:13} rows {f['rows']}  bars {f['bars']}  {f['first']}..{f['last']}  "
              f"null rows {f['null_rows']} (weekend {f['null_rows_on_weekends']}, weekday {f['null_rows_on_weekdays']})  "
              f"zero-volume bars {f['zero_volume_bars']}")
        print(f"{'':13} adj==close everywhere: {f['adj_equals_close_everywhere']}  adjustment steps: {f['adjustment_steps']}  "
              f"first Adj/Close {f['first_adj_over_close']}")
        print(f"{'':13} largest |close-to-close| %: {f['largest_moves']}   after the known-bad exclusions: {f['largest_moves_after_exclusions']}")
        if f["null_weekday_dates"]:
            print(f"{'':13} null rows on weekdays: {', '.join(f['null_weekday_dates'])}")
    print("== alignment of the five-file calendar (4 stocks + ^NSEI) ==")
    for k, v in align.items():
        if k not in ("stock_but_no_index_dates", "files"):
            print(f"  {k}: {v}")
    print("  dates with a stock bar but no ^NSEI bar:", ", ".join(align["stock_but_no_index_dates"]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
