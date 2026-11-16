#!/usr/bin/env python3
"""Download daily OHLCV for NSE tickers from Yahoo Finance into CSV files BacktestX can load.

Python standard library only (no pip packages).

    python tools/fetch_data.py                       # default tickers, 2010-01-01 .. today -> data/
    python tools/fetch_data.py --symbols TCS.NS ^NSEI --start 2015-01-01 --out data

Uses the unofficial Yahoo "v8 chart" JSON endpoint. It is undocumented, can change or rate-limit
without notice, and Yahoo's terms restrict redistribution: use the files for your own research only.
Not investment advice.

Output columns match the C++ loader: Date,Open,High,Low,Close,Adj Close,Volume.
Days where Yahoo returns a timestamp but no prices are written as `null` fields (the loader's
--skip-null policy decides what to do with them). A leading '^' is dropped in file names
(^NSEI -> NSEI.csv) because '^' is an escape character in cmd.exe.

Partial-bar rule: NSE's cash session ends 15:30 IST. If this script runs before 15:30 IST, any row
dated "today" (IST) is an unfinished bar and is dropped (and the drop is printed). From 15:30 IST on,
today's row is kept. The check uses the real clock; tests inject a fake one.

Exit status: 0 if every symbol was written, 1 if any failed.
"""
import argparse
import datetime as dt
import json
import os
import sys
import time
import urllib.error
import urllib.parse
import urllib.request

# NIFTYBEES.NS (an ETF tracking the Nifty 50) is the default BENCHMARK: unlike the ^NSEI price index it carries the
# fund's accumulated income, so it is a (noisy) total-return proxy. ^NSEI stays available.
DEFAULT_SYMBOLS = ["RELIANCE.NS", "TCS.NS", "HDFCBANK.NS", "INFY.NS", "^NSEI", "NIFTYBEES.NS"]
HEADER = "Date,Open,High,Low,Close,Adj Close,Volume"
URL = ("https://query1.finance.yahoo.com/v8/finance/chart/{sym}"
       "?period1={p1}&period2={p2}&interval=1d&events=div%2Csplits&includeAdjustedClose=true")
USER_AGENT = "Mozilla/5.0 (Windows NT 10.0; Win64; x64) BacktestX-fetch/1.0"
EPOCH = dt.datetime(1970, 1, 1)
IST = dt.timedelta(hours=5, minutes=30)
MARKET_CLOSE_IST = dt.time(15, 30)


class FetchError(Exception):
    pass


def _num(x):
    """Price field -> text. None/NaN -> 'null'. Rounded to 6 dp (Yahoo returns float32 noise)."""
    if x is None or x != x:
        return "null"
    return repr(round(float(x), 6))


def _vol(x):
    if x is None or x != x:
        return "null"
    return str(int(x))


def parse_chart(payload, symbol="?", warn=None):
    """Yahoo v8 chart JSON (already decoded) -> list of CSV data rows, ascending by date.

    * Date = (timestamp + meta.gmtoffset) as a calendar date, i.e. the exchange-local date.
    * Same date twice (Yahoo sometimes repeats the live day): the later entry wins.
    * No 'adjclose' block at all: Adj Close := Close, with a warning (never silent).
    """
    warn = warn or (lambda m: print("warning: " + m, file=sys.stderr))
    chart = payload.get("chart") or {}
    if chart.get("error"):
        raise FetchError("%s: Yahoo error: %s" % (symbol, chart["error"]))
    results = chart.get("result")
    if not results:
        raise FetchError("%s: empty result" % symbol)
    res = results[0]
    stamps = res.get("timestamp") or []
    quote = ((res.get("indicators") or {}).get("quote") or [{}])[0]
    adjblock = (res.get("indicators") or {}).get("adjclose")
    adj = adjblock[0].get("adjclose") if adjblock else None
    if adj is None:
        warn("%s: no adjclose in response; using Close as Adj Close" % symbol)
    off = int((res.get("meta") or {}).get("gmtoffset") or 0)

    by_date = {}
    for i, ts in enumerate(stamps):
        day = (EPOCH + dt.timedelta(seconds=int(ts) + off)).date().isoformat()
        o, h, l, c, v = (quote.get(k, [None] * len(stamps))[i] for k in ("open", "high", "low", "close", "volume"))
        a = adj[i] if adj is not None else c
        by_date[day] = ",".join([day, _num(o), _num(h), _num(l), _num(c), _num(a), _vol(v)])
    return [by_date[d] for d in sorted(by_date)]


def drop_partial_today(rows, now_utc):
    """Drop the row dated today (IST) if it is before 15:30 IST. now_utc: naive datetime in UTC.

    Returns (rows, dropped_date_or_None). Rows are CSV strings starting with YYYY-MM-DD.
    """
    now_ist = now_utc + IST
    if now_ist.time() >= MARKET_CLOSE_IST:
        return rows, None
    today = now_ist.date().isoformat()
    kept = [r for r in rows if r[:10] != today]
    return kept, (today if len(kept) != len(rows) else None)


def file_name(symbol):
    return symbol.lstrip("^") + ".csv"


def fetch(symbol, start, end, attempts=3):
    p1 = int((dt.datetime.combine(start, dt.time()) - EPOCH).total_seconds())
    p2 = int((dt.datetime.combine(end, dt.time()) - EPOCH).total_seconds()) + 86400
    url = URL.format(sym=urllib.parse.quote(symbol, safe=""), p1=p1, p2=p2)
    last = None
    for k in range(attempts):
        try:
            req = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
            with urllib.request.urlopen(req, timeout=30) as r:
                return json.loads(r.read().decode("utf-8"))
        except urllib.error.HTTPError as e:
            last = "HTTP %d" % e.code
            if e.code not in (429, 500, 502, 503, 504):
                break
        except (urllib.error.URLError, TimeoutError, json.JSONDecodeError) as e:
            last = str(e)
        time.sleep(2 * (k + 1))
    raise FetchError("%s: download failed (%s)" % (symbol, last))


def write_atomic(path, rows):
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf-8", newline="\n") as f:
        f.write(HEADER + "\n")
        for r in rows:
            f.write(r + "\n")
    os.replace(tmp, path)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--symbols", nargs="+", default=DEFAULT_SYMBOLS)
    ap.add_argument("--start", default="2010-01-01", help="YYYY-MM-DD")
    ap.add_argument("--end", default=dt.date.today().isoformat(), help="YYYY-MM-DD (inclusive)")
    ap.add_argument("--out", default="data", help="output directory (default: data)")
    a = ap.parse_args(argv)
    start, end = dt.date.fromisoformat(a.start), dt.date.fromisoformat(a.end)
    os.makedirs(a.out, exist_ok=True)

    failed = 0
    for sym in a.symbols:
        try:
            rows = parse_chart(fetch(sym, start, end), sym)
            now_utc = dt.datetime.now(dt.timezone.utc).replace(tzinfo=None)
            rows, dropped = drop_partial_today(rows, now_utc)
            if dropped:
                print("%-12s dropped partial bar dated %s (before 15:30 IST, session not finished)"
                      % (sym, dropped))
            if not rows:
                raise FetchError("%s: no rows returned" % sym)
            path = os.path.join(a.out, file_name(sym))
            write_atomic(path, rows)
            nulls = sum(1 for r in rows if "null" in r)
            print("%-12s %5d rows (%d with null fields)  %s .. %s -> %s"
                  % (sym, len(rows), nulls, rows[0][:10], rows[-1][:10], path))
        except FetchError as e:
            failed += 1
            print("ERROR: %s" % e, file=sys.stderr)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
