# ARCHITECTURE

Not investment advice. Past performance does not predict future results.

This file describes how BacktestX is organised and why. Formulas for every metric are in `LEARNING.md` (Phase 4) and the header comments of
`include/bt/analytics.hpp`.

## 1. File map

`bt_core` is a static library holding all logic; `backtestx` (the CLI), `bt_tests` and `bt_bench` link against it. Headers are in
`include/bt/`, sources in `src/`.

| layer | files | what they do |
|---|---|---|
| dates and bars | `date`, `bar`, `types` | a date is days since 1970-01-01 (`int32`); a `Bar` is one day of OHLCV; `Side` |
| reading data | `csv`, `adjust`, `dataset`, `calendar` | strict CSV loader (line-numbered errors); adjusted-price computation; per-file validation (excluded dates, implausible-move guard); the union calendar of all loaded symbols (`AlignedData`) |
| money and costs | `money`, `costs` | `int64` paise, exact rate arithmetic (parts per billion, ties away from zero); the NSE fee model and slippage |
| orders and fills | `order`, `fill`, `portfolio` | order types and lifecycle; the fill simulator; cash, positions, round-trip (trade) tracking |
| the engine | `strategy`, `engine` | `HistoryView`, `Context`, the `Strategy` interface; the day loop |
| strategies | `indicators`, `sizing`, `strategies` | incremental SMA / RSI / ATR / Donchian / rolling-return indicators; position sizing; the five built-in strategies and their factory |
| analytics | `analytics`, `export` | every metric as a documented function of the exported rows; CSV and JSON writers |
| configuration and runs | `json_util`, `run_config`, `runner` | strict JSON helpers (unknown keys are errors); the run config parser; load data, window it, run, write the run folder |
| sweeps | `sweep` | grid expansion, thread pool, selection, in-sample / out-of-sample windows, overfit warnings, `sweep.csv` / `sweep_summary.json` |
| report | `report`, `report_assets`, `src/report_assets/*` | builds `report.html` from a run or sweep folder; the HTML/CSS/JS are real files embedded at configure time |
| command line | `commands`, `cli/main.cpp` | `run`, `sweep`, `report`, `check`, `synth`; `main` is a thin dispatcher so tests can call the commands in-process |

Outside `src/`: `tests/` (doctest), `bench/bench_main.cpp`, and `tools/` (Python and PowerShell: the independent metrics reference
`ref_metrics.py`, `check_report.py`, `mutate.py`, data fetching and the analysis scripts in `tools/analysis/`).

## 2. One bar's lifecycle

For every calendar day `t` of the union calendar the engine does, in this order:

1. **Fill.** Orders that were submitted on an earlier day are tried against today's bar for every symbol that traded today. They are processed
   Market first, then Stop, then Limit, ties by ascending order id. Fill price, slippage, the volume-participation cap and the fees
   (`costs`) are applied; cash and positions change in `Portfolio`, and a flat-to-flat sequence of fills becomes a `Trade`. Day orders
   that did not fill expire here; good-till-cancelled orders stay pending.
2. **Mark to market.** Every held position is valued at the last known close (a symbol that did not trade keeps its previous close) and an
   equity record `(date, cash, positions value, equity)` is appended.
3. **Decide.** `Strategy::on_bar(Context)` runs. It sees each symbol through a `HistoryView` of bars up to and including today's close and may
   stage orders (`market`, `stop`, `limit`, `cancel`).
4. **Collect.** The staged orders are validated (positive size, no shorting unless enabled, a single fill cannot cross zero) and become pending.
   They can first execute on a LATER day's bar, never on the bar the decision was made on.

**Warm-up.** If an evaluation start date is given, days before it still run step 3 (so indicators see history and `Context::in_warmup()` is
true) but nothing counts: orders staged are discarded, no fills or equity records exist, the portfolio stays at initial cash, and order ids
restart on the first evaluation day. The first decision that counts is made at the close of the first evaluation day.

After the loop the equity curve, fills and trades go to `analytics::analyze`, then `export_run` writes `equity.csv`, `trades.csv`, `fills.csv`,
`benchmark.csv` and `summary.json`; `runner` adds `run.json`; `report` reads those files and writes `report.html`.

## 3. Why look-ahead is impossible (and what "impossible" means here)

- A strategy never receives a `std::vector<Bar>`. It receives a `HistoryView` that stores only a pointer to the series and the **count of
  visible bars**; `operator[]` throws `std::out_of_range` beyond that count and there is no iterator, no `data()` and no way to obtain a
  pointer to a later bar. `AlignedData::bar(symbol, t)` and the per-symbol visible counts are what the engine uses to build it.
- Orders staged on day `t` cannot execute before day `t + 1`, so a decision cannot trade at a price it has already seen.
- Indicators are incremental and are fed only the bars the view exposes.
- In sweeps the in-sample data is **physically cut** at the split (`make_aligned(series, last_date)`), so the in-sample runs cannot read an
  out-of-sample bar even by a bug, and the selection step receives only in-sample metrics.
- The guarantee is at the API level: code that casts pointers around could defeat it. It is checked by tests, not proved: `test_lookahead`
  runs the strategies on data where every bar after day `t` has been replaced by garbage and requires that nothing decided on or before `t`
  changes (with a deliberately cheating strategy as a positive control, and also with a warm-up window); `test_sweep` does the same for the
  out-of-sample part of a sweep.

## 4. Threading model of sweeps

- A sweep is a grid of independent backtests over the same **read-only** `AlignedData`. `expand_grid` builds the list of parameter points in a
  deterministic order (axes alphabetical, last axis varying fastest).
- `run_sweep` starts `min(threads, runnable points)` workers. Workers take the next grid index from a shared counter, run
  `run_backtest` with their **own** strategy object and their **own copy** of the parameter JSON (nothing mutable is shared), and write the
  result into the slot for that grid index, never in completion order. Exceptions are caught in the worker and become a row with status
  `error`, so one bad combination cannot kill the sweep.
- Because results are stored by index and each run is deterministic, the output is identical for any thread count; each row carries an
  FNV-1a hash of its full result (equity records, fills, trades, events), and tests compare 1, 2, 4 and N threads with a plain sequential
  loop. `bt_bench` asserts the same before it reports any throughput.
- Selection (`select_best`) happens after all workers have finished and uses only in-sample metrics.
- There is no ThreadSanitizer on MinGW; the determinism tests are the evidence for thread safety, not a proof of it.
