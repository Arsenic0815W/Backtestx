#!/usr/bin/env python3
"""Why do raw-price and adjusted-price SMA-crossover runs give different trade counts?

    python tools/analysis/raw_vs_adjusted_signals.py [--symbol RELIANCE.NS] [--fast 20] [--slow 50] [--data DIR]

Adjustment multiplies each bar by f = Adj Close / Close, and f steps at every dividend. If f is constant across an indicator's whole
window, adjusted prices are the raw prices times a constant, so "fast SMA > slow SMA" is true on exactly the same days. It can differ
only when a step falls inside the window. This script measures that on a real file: the number of f steps, the days on which the
crossover state differs between raw and adjusted, how many of those days have a step inside the last SLOW bars, whether the states
agree on EVERY day without a step in the window, and the number of state changes (= fills of a long-only crossover) for each basis.
Source of the numbers quoted in LEARNING.md (Phase 3 review amendments). Skips cleanly without the real file.
"""
import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))  # needed under `python -I`
import _common as C  # noqa: E402


def sma(x, i, k):
    return sum(x[i - k + 1:i + 1]) / k


def analyse(bars, fast, slow):
    close = [b.close for b in bars]
    adj = [b.adj for b in bars]
    ratio = [a / c for a, c in zip(adj, close)]
    n = len(bars)
    steps = [i for i in range(1, n) if abs(ratio[i] / ratio[i - 1] - 1) > 1e-5]

    def step_in_window(i):
        return any(i - slow + 1 <= s <= i for s in steps)

    idx = range(slow - 1, n)
    raw_state = [sma(close, i, fast) > sma(close, i, slow) for i in idx]
    adj_state = [sma(adj, i, fast) > sma(adj, i, slow) for i in idx]
    differ = [i for i, r, a in zip(idx, raw_state, adj_state) if r != a]
    no_step_days_agree = all(r == a for i, r, a in zip(idx, raw_state, adj_state) if not step_in_window(i))
    changes = lambda s: sum(1 for a, b in zip(s, s[1:]) if a != b)
    return {
        "bars": n,
        "adjustment_steps": len(steps),
        "days_state_differs": len(differ),
        "of_which_with_a_step_in_the_window": sum(1 for i in differ if step_in_window(i)),
        "states_agree_on_every_day_without_a_step_in_the_window": no_step_days_agree,
        "state_changes_raw": changes(raw_state),
        "state_changes_adjusted": changes(adj_state),
        "first_adj_over_close": round(ratio[0], 4),
    }


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--symbol", default="RELIANCE.NS")
    ap.add_argument("--fast", type=int, default=20)
    ap.add_argument("--slow", type=int, default=50)
    ap.add_argument("--data", default=C.DEFAULT_DATA)
    a = ap.parse_args()
    paths = C.require_files(a.data, [a.symbol])
    bars, nulls = C.load_csv(paths[a.symbol])
    r = analyse(bars, a.fast, a.slow)
    print(f"{a.symbol}: {r['bars']} bars after skipping {len(nulls)} null rows; SMA({a.fast}) > SMA({a.slow})")
    print(f"  days where the adjustment factor f = Adj/Close steps (ex-dividend-like): {r['adjustment_steps']}")
    print(f"  days where the crossover state differs between raw and adjusted: {r['days_state_differs']}")
    print(f"    of those, days with a step inside the last {a.slow} bars: {r['of_which_with_a_step_in_the_window']}")
    print(f"  states agree on EVERY day that has no step in the window: {r['states_agree_on_every_day_without_a_step_in_the_window']}")
    print(f"  state changes (= fills of a long-only crossover): raw {r['state_changes_raw']}, adjusted {r['state_changes_adjusted']}")
    print(f"  Adj Close / Close on the first bar: {r['first_adj_over_close']}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
