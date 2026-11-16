# Sample data — SYNTHETIC, not real prices

These files exist so every command works offline. They are **seeded geometric Brownian motion**, not
NSE data. The names (`SYNTH_*`) are deliberate so they cannot be mistaken for real tickers.

| File | Bars | Range | Generated with |
|---|---|---|---|
| `SYNTH_A.csv` | 500 | 2022-01-03 .. 2023-12-01 | `backtestx synth --seed 1 --bars 500 --start 2022-01-03 --price 1500` |
| `SYNTH_B.csv` | 500 | 2022-01-03 .. 2023-12-01 | `backtestx synth --seed 2 --bars 500 --start 2022-01-03 --price 800` |
| `SYNTH_C.csv` | 400 | 2022-07-01 .. 2024-01-11 | `backtestx synth --seed 3 --bars 400 --start 2022-07-01 --price 3200` |

Defaults used: drift 8 %/yr, volatility 25 %/yr, base volume 1,000,000, `Adj Close` == `Close`.
Calendar is Monday–Friday only (no NSE holidays). `SYNTH_C` starts later than the others on purpose,
to exercise multi-symbol calendar alignment.

Real data: `python tools/fetch_data.py` downloads Yahoo Finance daily data into `data/` (not here).
Yahoo's terms restrict redistribution, so downloaded files are for your own research and are not
part of the shipped sample set.

Not investment advice. Past performance does not predict future results.
