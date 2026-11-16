"""Shared helpers for the analysis scripts in tools/analysis/ (Python standard library only).

STANDING RULE: any number or finding cited in a report or in LEARNING.md must come from a script in this folder, not from a
scratch file. Scripts that need the real Yahoo files (data/*.csv, from `python tools/fetch_data.py`) SKIP CLEANLY when none of
the required files exist (message + exit 0) and FAIL (exit 1) when they are only partly present.
"""
import csv
import datetime as dt
import json
import math
import os
import sys
from collections import namedtuple

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
DEFAULT_DATA = os.path.join(REPO, "data")
KNOWN_BAD_FILE = os.path.join(HERE, "known_bad_bars.json")

Bar = namedtuple("Bar", "date open high low close adj volume")


def file_name(symbol):
    """Same rule as tools/fetch_data.py and the C++ config: a leading '^' is dropped."""
    return symbol.lstrip("^") + ".csv"


def load_csv(path):
    """Returns (bars, null_dates): bars sorted as in the file, null rows (Yahoo's all-null days) separated."""
    bars, nulls = [], []
    with open(path, newline="", encoding="utf-8") as f:
        for r in csv.DictReader(f):
            d = dt.date.fromisoformat(r["Date"])
            if r["Close"] == "null":
                nulls.append(d)
            else:
                bars.append(Bar(d, float(r["Open"]), float(r["High"]), float(r["Low"]), float(r["Close"]),
                                float(r["Adj Close"]), int(r["Volume"])))
    return bars, nulls


def known_bad_bars():
    """{symbol: [date, ...]}: the closes found to be corrupt by inspection (single source of truth, also read by
    tools/realdata_check.ps1). The list is NOT exhaustive."""
    with open(KNOWN_BAD_FILE, encoding="utf-8") as f:
        raw = json.load(f)
    return {k: [dt.date.fromisoformat(x) for x in v] for k, v in raw.items() if not k.startswith("_")}


def require_files(data_dir, symbols):
    """Skip cleanly (exit 0) if NONE of the files exist; fail (exit 1) if only some do. Returns {symbol: path}."""
    paths = {s: os.path.join(data_dir, file_name(s)) for s in symbols}
    present = [s for s, p in paths.items() if os.path.exists(p)]
    if not present:
        print(f"SKIPPED: none of the real data files were found in '{data_dir}'.")
        print("         Expected: " + ", ".join(os.path.basename(p) for p in paths.values()))
        print("         Download them with: python tools/fetch_data.py   (real data is for your own research; it is not shipped)")
        sys.exit(0)
    if len(present) != len(symbols):
        missing = [os.path.basename(paths[s]) for s in symbols if s not in present]
        print(f"ERROR: real data is only partly present in '{data_dir}'. Missing: {', '.join(missing)}")
        sys.exit(1)
    return paths


def cagr(first, last, days):
    years = days / 365.25
    return (last / first) ** (1.0 / years) - 1.0


def sample_sd(v):
    m = sum(v) / len(v)
    return math.sqrt(sum((x - m) ** 2 for x in v) / (len(v) - 1))


def correlation(a, b):
    ma, mb = sum(a) / len(a), sum(b) / len(b)
    cov = sum((x - ma) * (y - mb) for x, y in zip(a, b)) / (len(a) - 1)
    return cov / (sample_sd(a) * sample_sd(b))


def pct(x, nd=2):
    return f"{x * 100:.{nd}f}%"
