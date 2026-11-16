# data/

Real daily data downloaded by `python tools/fetch_data.py` (Yahoo Finance, unofficial endpoint).
For your own research only: Yahoo's terms restrict redistribution, so these files are **not** shipped
(`data/sample/` holds the synthetic files that ship). No test depends on anything in this folder.

Files: `RELIANCE.NS.csv`, `TCS.NS.csv`, `HDFCBANK.NS.csv`, `INFY.NS.csv`, `NSEI.csv` (= `^NSEI`; the `^` is
dropped from the file name because it is an escape character in cmd.exe) and `NIFTYBEES.NS.csv` (the default benchmark,
see below).

Last fetched 2026-10-28 ~13:20 IST. The fetch tool drops a row dated "today" (IST) when run before
15:30 IST, so these files end on **2026-10-27** (4,158 rows each, 2010-01-04 .. 2026-10-27). `NIFTYBEES.NS.csv` was
fetched separately with `--symbols NIFTYBEES.NS --end 2026-10-27` so that it ends on the same date and the five existing
files were not overwritten by a time-dependent re-download.

Load with `NullPolicy::Skip` (CLI: `--skip-null`); the strict default rejects them (see below).

## Known gaps: null rows in the data

Yahoo returns a timestamp with every value `null` on some days. They are skipped, not repaired.

**The four stocks** have identical null rows (7): 2010-02-06, 2012-01-07, 2012-03-03, 2012-09-08,
2012-11-11, 2014-03-22, 2015-02-28. All fall on a Saturday/Sunday (likely special weekend sessions;
not confirmed).

**`^NSEI`** has 28 null rows: the 7 above, plus 2019-10-27 (Sun) and 2020-11-14 (Sat), plus
**19 on ordinary weekdays**, left as skipped (the stocks DO have real bars on these days, so on these
dates the index is simply missing):

| Date | Day | | Date | Day |
|---|---|---|---|---|
| 2011-05-31 | Tue | | 2014-10-15 | Wed |
| 2011-07-14 | Thu | | 2015-01-01 | Thu |
| 2011-11-24 | Thu | | 2015-04-15 | Wed |
| 2011-12-26 | Mon | | 2016-01-01 | Fri |
| 2012-01-02 | Mon | | 2016-08-12 | Fri |
| 2012-05-21 | Mon | | 2018-01-01 | Mon |
| 2012-08-28 | Tue | | 2019-01-01 | Tue |
| 2013-01-01 | Tue | | 2026-01-15 | Thu |
| 2014-01-01 | Wed | | | |
| 2014-02-17 | Mon | | | |
| 2014-04-24 | Thu | | | |

Why they are missing was not investigated. Consequence for analytics: on these dates there is no
benchmark return; see the pairwise-intersection rule in `LEARNING.md` (Phase 1 follow-up decisions).

Also: `^NSEI` has many rows with volume 0 (an index has no traded volume); that is valid data.

## NIFTYBEES.NS (default benchmark): what the file really is

NIFTYBEES.NS is an ETF that tracks the Nifty 50. It is the default benchmark because the `^NSEI` index is a *price* index
(no dividends) while adjusted stock prices include dividends, so alpha against `^NSEI` is biased upward by roughly the
dividend yield. An ETF's price carries the fund's accumulated income, so it is a **proxy** for a total-return benchmark, not
an exact one: the fund has an expense ratio (not quantified here) and tracking error against the index.

Measured on this file with a one-off script (not kept in the repo), 4,158 rows, 2010-01-04 .. 2026-10-27:

- **`Adj Close` equals `Close` on every row** (ratio 1.0000, zero adjustment steps): Yahoo applies no dividend adjustment to
  this ETF, so the "adjusted" price basis changes nothing for it.
- **9 null rows**: 6 on weekends (2010-02-06, 2012-01-07, 2012-03-03, 2012-09-08, 2012-11-11, 2014-03-22) and **3 on weekdays**
  (2014-04-24, 2014-10-15, 2025-10-24). They are skipped (`"null_rows": "skip"`).
- **Four corrupted closes**, found by inspection (neighbouring bars are on a different price level):
  2010-02-05 (close 54.50 between ~49 and ~48), 2010-10-06 (close 52.50 between ~62 and ~62),
  and 2019-12-19 / 2019-12-20 (prices around 13.0 between neighbours around 129-131: a -90 % day and then a +897 % day).
  Left in, they destroy beta, alpha and correlation (correlation with `^NSEI` 0.06, tracking error 223 %/yr).
  The CSV is never edited; the run config excludes these dates explicitly (`data.exclude_dates`), and the run records them.
- With the four closes excluded: CAGR 9.99 % vs 9.14 % for `^NSEI` (**+0.85 %/yr**), annualised tracking error **5.0 %**,
  daily-return correlation **0.95** (days between consecutive common dates, 4,125 dates). That is far noisier than a real ETF
  would be (a few days, e.g. 2020-03-16, differ from the index by more than 3 points). That the +0.85 %/yr gap is dividends is a
  plausible reading, **not established**: non-synchronous closing prices could contribute, and nothing in the file says how the
  fund treats distributions.
- **The exclusion list is not exhaustive.** An implausible-move guard (`data.max_daily_move`, default 0.15 for a benchmark,
  0.5 for traded symbols) fails the run loudly on large jumps, but it cannot see small bad ticks: the 2010-02-05 close moves
  only +10.8 % then -11.5 %, below any sane threshold.
- Largest real close-to-close moves, for calibrating the guard: RELIANCE 14.7 %, TCS 12.2 %, HDFCBANK 12.6 %, INFY 21.3 %
  (2013-04-12), `^NSEI` 13.0 %, NIFTYBEES 10.2 % (after the exclusions).

Not investment advice. Past performance does not predict future results.
