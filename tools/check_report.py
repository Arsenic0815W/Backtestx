#!/usr/bin/env python3
"""Independent check of a report.html against the files in its own folder (pure Python, standard library only).

    python -I tools/check_report.py DIR [DIR ...]      DIR = a run folder or a sweep folder (a sweep's best/ is checked too)

Static checks on the HTML: the disclaimer is present, no external reference of any kind (http(s) other than the SVG namespace,
@import, url(, src=, href to a scheme), exactly the expected <script> elements, the embedded JSON is ASCII with no '<'.
Content checks, recomputed here WITHOUT the C++ report code: the embedded summary equals summary.json / sweep_summary.json; the
equity curve, drawdown series, monthly and yearly returns, fee totals, trade rows, open positions and the rebased benchmark curve
are re-derived from the CSV files; the benchmark's own metrics are recomputed with tools/ref_metrics.py. Exit 0 = all agree.
"""
import csv
import json
import math
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))  # needed under `python -I`
import ref_metrics as RM  # noqa: E402

problems = []


def bad(msg):
    problems.append(msg)


def load_html(path):
    with open(path, encoding="utf-8", newline="") as f:
        return f.read()


def embedded(html):
    m = re.search(r'<script type="application/json" id="bt-data">(.*?)</script>', html, re.S)
    if not m:
        bad("no <script id=bt-data> element")
        return None
    raw = m.group(1)
    if "<" in raw:
        bad("embedded JSON contains a raw '<'")
    if any(ord(c) > 127 for c in raw):
        bad("embedded JSON is not pure ASCII")
    return json.loads(raw)


def static_checks(html, path):
    if "Not investment advice. Past performance does not predict future results." not in html:
        bad(f"{path}: disclaimer missing")
    if html.count("</script>") != 2:
        bad(f"{path}: expected exactly 2 </script> (data + code), found {html.count('</script>')}")
    for m in re.finditer(r"https?://[^\s\"'<>)]*", html):
        if m.group(0) != "http://www.w3.org/2000/svg":
            bad(f"{path}: external URL {m.group(0)}")
    for pat in ("@import", "url(", "src=", "<link", "<iframe", "<img", "fetch(", "XMLHttpRequest", "navigator.sendBeacon"):
        if pat in html:
            bad(f"{path}: contains {pat!r}")
    if "innerHTML" in html:
        bad(f"{path}: JS uses innerHTML")


def close(a, b, tol=1e-12):
    if a is None or b is None:
        return a is None and b is None
    return abs(a - b) <= tol * max(1.0, abs(a), abs(b))


def read_rows(path):
    with open(path, newline="", encoding="utf-8") as f:
        return list(csv.DictReader(f))


def month_key(s):
    return s[:7]


def check_run(folder, html_path):
    html = load_html(html_path)
    static_checks(html, html_path)
    D = embedded(html)
    if D is None:
        return
    with open(os.path.join(folder, "summary.json"), encoding="utf-8") as f:
        summary = json.load(f)
    if D["summary"] != summary:
        bad(f"{folder}: embedded summary differs from summary.json")
    eq = read_rows(os.path.join(folder, "equity.csv"))
    dates = [r["date"] for r in eq]
    E = [int(r["equity_paise"]) for r in eq]
    if D["equity"]["dates"] != dates or D["equity"]["equity_paise"] != E:
        bad(f"{folder}: equity series differs from equity.csv")

    def drawdown(series):
        out, peak = [], None
        for e in series:
            peak = e if peak is None or e > peak else peak
            out.append(e / peak - 1.0 if peak > 0 else None)
        return out

    dd = drawdown(E)
    if not all(close(a, b) for a, b in zip(D["equity"]["drawdown"], dd)) or len(dd) != len(D["equity"]["drawdown"]):
        bad(f"{folder}: drawdown series differs")

    # monthly / yearly, derived from scratch
    def periods(keyfn):
        out, base, base_date, i = [], E[0], dates[0], 0
        while i < len(dates):
            j = i
            while j + 1 < len(dates) and keyfn(dates[j + 1]) == keyfn(dates[i]):
                j += 1
            r = E[j] / base - 1.0 if base > 0 and base_date != dates[j] else None
            out.append((keyfn(dates[i]), r, base_date, dates[j]))
            base, base_date, i = E[j], dates[j], j + 1
        return out

    mon = periods(month_key)
    if len(mon) != len(D["monthly"]):
        bad(f"{folder}: monthly count {len(D['monthly'])} != {len(mon)}")
    else:
        for (k, r, f0, t0), m in zip(mon, D["monthly"]):
            if f"{m['y']:04d}-{m['m']:02d}" != k or not close(r, m["r"]) or m["from"] != f0 or m["to"] != t0:
                bad(f"{folder}: monthly mismatch at {k}")
        if not D["monthly"][0]["first"] or not D["monthly"][-1]["last"]:
            bad(f"{folder}: first/last month flags missing")
    yrs = periods(lambda s: s[:4])
    if len(yrs) != len(D["yearly"]) or any(str(y["y"]) != k or not close(r, y["r"]) for (k, r, _, _), y in zip(yrs, D["yearly"])):
        bad(f"{folder}: yearly returns differ")
    # compounded months == year
    for (k, r, _, _), y in zip(yrs, D["yearly"]):
        ms = [m["r"] for m in D["monthly"] if str(m["y"]) == k]
        if None not in ms and r is not None and ms:
            p = 1.0
            for x in ms:
                p *= 1.0 + x
            if not close(p - 1.0, r, 1e-9):
                bad(f"{folder}: year {k} is not the compounded months")

    # fees
    fills = read_rows(os.path.join(folder, "fills.csv"))
    comps = ["brokerage_paise", "stt_paise", "exchange_paise", "sebi_paise", "stamp_paise", "gst_paise"]
    totals = {c: sum(int(r[c]) for r in fills) for c in comps}
    total = sum(int(r["fees_paise"]) for r in fills)
    F = D["fees"]
    if F["total_paise"] != total or total != summary["trades"]["total_fees_paise"] or sum(totals.values()) != total:
        bad(f"{folder}: fee total mismatch")
    for c in F["components"]:
        if c["paise"] != totals[c["key"]]:
            bad(f"{folder}: fee component {c['key']} differs")
    if F["by_side"]["buy_paise"] != sum(int(r["fees_paise"]) for r in fills if r["side"] == "buy") or \
            F["by_side"]["sell_paise"] != sum(int(r["fees_paise"]) for r in fills if r["side"] == "sell"):
        bad(f"{folder}: fees by side differ")
    bysym = {}
    for r in fills:
        bysym.setdefault(r["symbol"], [0, 0, 0])
        b = bysym[r["symbol"]]
        b[0] += 1
        b[1] += int(r["notional_paise"])
        b[2] += int(r["fees_paise"])
    if {s["symbol"]: [s["fills"], s["notional_paise"], s["paise"]] for s in F["by_symbol"]} != bysym:
        bad(f"{folder}: fees by symbol differ")
    last_pos = {}
    for r in fills:
        last_pos[r["symbol"]] = int(r["position_after"])
    if {p["symbol"]: p["qty"] for p in D["open_positions"]} != {s: q for s, q in last_pos.items() if q != 0}:
        bad(f"{folder}: open positions differ")
    if len(D["open_positions"]) != summary["trades"]["num_open_positions"]:
        bad(f"{folder}: open positions != summary num_open_positions")

    # trades
    trades = read_rows(os.path.join(folder, "trades.csv"))
    T = D["trades"]
    if len(T["rows"]) != len(trades) or len(trades) != summary["trades"]["num_trades"]:
        bad(f"{folder}: trade count differs")
    else:
        ix = {c: i for i, c in enumerate(T["columns"])}
        for row, t in zip(T["rows"], trades):
            if row[ix["net_pnl_paise"]] != int(t["net_pnl_paise"]) or row[ix["symbol"]] != t["symbol"] or row[ix["entry_date"]] != t["entry_date"]:
                bad(f"{folder}: a trade row differs")
                break
            ev = int(t["entry_value_paise"])
            if not close(row[ix["return"]], int(t["net_pnl_paise"]) / ev if ev else None):
                bad(f"{folder}: a trade return differs")
                break

    # P&L split: closed (sum of trades.csv net) + unrealised = total = end equity - start equity
    closed = sum(int(t["net_pnl_paise"]) for t in trades)
    P = D["pnl"]
    if P["closed_trades_net_paise"] != closed or P["total_paise"] != E[-1] - E[0] or P["closed_trades_net_paise"] + P["unrealised_paise"] != P["total_paise"]:
        bad(f"{folder}: P&L split (closed + unrealised = total) differs")
    if summary["trades"]["net_pnl_paise"] != P["total_paise"]:
        bad(f"{folder}: total P&L differs from summary.json net_pnl_paise")
    if not D["open_positions"] and P["unrealised_paise"] != 0:
        bad(f"{folder}: no open positions but unrealised P&L is not zero")

    # benchmark: rebase and re-measure with the Python reference
    bpath = os.path.join(folder, "benchmark.csv")
    B = D["benchmark"]
    if os.path.exists(bpath):
        bench = [(r["date"], float(r["value"])) for r in read_rows(bpath)]
        eqd = dict(zip(dates, E))
        common = [(d, v) for d, v in bench if d in eqd]
        if not common:
            if B is not None:
                bad(f"{folder}: benchmark embedded though there are no common dates")
        elif B is None:
            bad(f"{folder}: benchmark missing from the report")
        else:
            e0, v0 = eqd[common[0][0]], common[0][1]
            exp = [math.floor(abs(e0 * v / v0) + 0.5) * (1 if e0 * v / v0 >= 0 else -1) for _, v in common]
            if B["dates"] != [d for d, _ in common]:
                bad(f"{folder}: benchmark dates differ")
            elif any(abs(a - b) > 1 for a, b in zip(B["equity_paise"], exp)):
                bad(f"{folder}: rebased benchmark values differ by more than 1 paisa")
            elif B["equity_paise"][0] != e0:
                bad(f"{folder}: benchmark does not start at the strategy equity")
            rows = [{"date": d, "cash_paise": "0", "positions_value_paise": str(p), "equity_paise": str(p)} for d, p in zip(B["dates"], B["equity_paise"])]
            ref = RM.equity_metrics(rows, summary["config"])
            for k, v in ref.items():
                a = B["metrics"].get(k)
                ok, _ = RM.compare_values(a, v)
                if not ok:
                    bad(f"{folder}: benchmark metric {k}: report {a!r} vs Python {v!r}")
            if sorted(B["metrics"]) != sorted(ref):
                bad(f"{folder}: benchmark metric key sets differ")
    elif B is not None:
        bad(f"{folder}: benchmark embedded but there is no benchmark.csv")

    # paths shortened
    run_text = json.dumps(D["run"])
    if re.search(r"[A-Za-z]:[/\\\\]|/Users/|/home/", run_text):
        bad(f"{folder}: an absolute path survived in the embedded run info")
    print(f"ok   run report {html_path}: {len(eq)} records, {len(trades)} trades, {len(fills)} fills, benchmark={'yes' if B else 'no'}")


def check_sweep(folder, html_path):
    html = load_html(html_path)
    static_checks(html, html_path)
    D = embedded(html)
    if D is None:
        return
    with open(os.path.join(folder, "sweep_summary.json"), encoding="utf-8") as f:
        ss = json.load(f)
    if D["summary"] != ss:
        bad(f"{folder}: embedded sweep summary differs from sweep_summary.json")
    rows = read_rows(os.path.join(folder, "sweep.csv"))
    if len(rows) != len(D["rows"]) or len(rows) != ss["grid"]["n_combinations"]:
        bad(f"{folder}: row count differs")
        return
    cols = D["columns"]
    if cols != list(rows[0].keys()):
        bad(f"{folder}: column list differs")
    for r, e in zip(rows, D["rows"]):
        for c, v in zip(cols, e):
            raw = r[c]
            if raw == "":
                if v is not None:
                    bad(f"{folder}: run {r['run_id']} column {c}: expected null")
            elif isinstance(v, (int, float)) and not isinstance(v, bool):
                if not close(float(raw), float(v), 1e-15):
                    bad(f"{folder}: run {r['run_id']} column {c}: {raw} vs {v}")
            elif v != raw:
                bad(f"{folder}: run {r['run_id']} column {c}: {raw!r} vs {v!r}")
    sel = [r for r in rows if r["selected"] == "1"]
    best = ss["best"]
    if (best is None) != (not sel) or (best and (len(sel) != 1 or int(sel[0]["run_id"]) != best["run_id"])):
        bad(f"{folder}: selected row disagrees with sweep_summary.json")
    if best:
        # the selected row must be the in-sample maximum among eligible rows (lowest id on ties)
        metric = "is_" + ss["selection_metric"]
        elig = [(float(r[metric]), -int(r["run_id"])) for r in rows
                if r["status"] == "ok" and r[metric] != "" and int(r["is_num_trades"]) >= ss["min_trades"]]
        if not elig or -max(elig)[1] != best["run_id"]:
            bad(f"{folder}: selected run is not the eligible in-sample maximum")
    print(f"ok   sweep report {html_path}: {len(rows)} combinations, best={best['run_id'] if best else None}")


def check_dir(folder):
    html_path = os.path.join(folder, "report.html")
    if not os.path.exists(html_path):
        bad(f"{folder}: no report.html")
        return
    if os.path.exists(os.path.join(folder, "sweep_summary.json")):
        check_sweep(folder, html_path)
        if os.path.isdir(os.path.join(folder, "best")):
            check_dir(os.path.join(folder, "best"))
    else:
        check_run(folder, html_path)


def main(argv):
    if not argv:
        print(__doc__)
        return 2
    for d in argv:
        check_dir(d)
    if problems:
        print("FAIL")
        for p in problems:
            print("  " + p)
        return 1
    print("report check: all agree")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
