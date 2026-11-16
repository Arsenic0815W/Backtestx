## Reading the numbers (hand-written; every figure below is copied from the tables above)

**Single backtest.** On this machine one 20-year, one-symbol `sma_crossover` backtest (5,040 bars, 59 trades) took a median of 0.359 to 0.361 ms
per run (14.0 million bars/s, a spread of under 1% between the three runs), and the 50-symbol, 10-year `momentum_rotation` backtest
(126,000 bars, 240 trades) 5.978 to 6.406 ms (about 20.5 million bars/s; the three runs differ by up to about 7%, from 19,669,060 to
21,079,046 bars/s, so treat the second figure as good to a few percent, not to three digits). These are throughput figures for this engine on this
machine only; nothing was compared with any other engine, so there is no claim here that it is fast or slow relative to anything.
Bars/sec is higher for the 50-symbol workload than for the one-symbol workload; that is a property of how the work splits between
per-day and per-symbol cost in this code, and I did not profile it, so I do not give a cause.

**Sweep scaling.** One grid point is one backtest (1 symbol x 2,520 bars, about a quarter of a millisecond of work), 128 points.
Median-of-runs throughput: 3,908 runs/s on 1 thread, 7,427 on 2 (1.90x, 95% efficiency), 13,282 on 4 (3.40x, 85%), 17,878 on 8 (4.58x, 57%).
The results of every grid point were bit-identical at every thread count and in every run (the signature in the table), so these
throughputs are throughputs of the same answer.

What the curve shows, and what I did NOT test:
- The machine has 4 physical cores and 8 logical processors. Speedup keeps rising up to 8 threads but flattens: going from 4 to 8 threads
  adds 1.35x (17,878 / 13,282), not 2x. **Hypothesis:** the 8 logical processors are 4 cores with two hardware threads each, which share
  execution units and caches, so the second thread per core adds much less than a full core. Not tested (it would need a run pinned to one
  thread per core).
- Efficiency also falls from 95% to 85% between 2 and 4 threads, where every thread can still have its own core. **Hypotheses**, none tested:
  contention on the memory allocator (each backtest allocates its own curves and vectors), memory bandwidth / shared cache, and the clock
  behaviour of a laptop CPU when more cores are active.
- **Tail effect and fixed costs:** with 128 short tasks, a whole sweep call takes only about 7.2 ms at 8 threads (128 / 17,878 s), so starting
  threads and the last tasks finishing unevenly are a visible share of the time. A grid of heavier backtests (more bars or symbols) would
  probably scale better; that is also untested here.
- In every sweep row the third run is the lowest of the three (for example 17,028 runs/s at 8 threads against 17,878 and 17,881 in runs 1 and
  2), and the same was seen in the earlier battery run at 4 threads. Each run used a different thread-count order, but the
  cause (order, drift over the session, or chance) was not tested.
- The thread-count order was rotated between runs (see the environment table) so a warming or throttling CPU does not systematically
  favour the first or last configuration; with three runs this reduces, but does not remove, that risk.

**Battery versus AC power.** The same benchmark was run twice on the same laptop (High performance power plan both times): first on battery,
then on AC power (see the comparison table above). The single-backtest figures changed by +1.7% and -0.9%, which is inside the run-to-run
spread of the 50-symbol workload. The sweep throughput rose with the thread count: +1.0% at 1 thread, +3.2% at 2, +6.3% at 4 and +13.4% at 8.
That pattern is what one would expect if the CPU is power-limited when many cores are busy on battery, but the clock speed was not
recorded during the runs, so the cause is a hypothesis. Only the AC run is shown in the tables; the battery figures are the transcribed
medians in `docs/bench_battery_baseline.json`.

## Limits of this benchmark

- One machine (an 8-thread laptop CPU), one compiler (the flags in the environment table), one operating system. The tables are from a run
  on **AC power** (power plan: High performance).
- Synthetic geometric-Brownian-motion data with weekday calendars; real files, missing-data handling and report writing are not included.
- Only two single-run workloads and one sweep shape were measured. Strategies differ in cost; `sma_crossover` and `momentum_rotation` do not
  represent the other three.
- The indicators' "amortised O(1) per bar" property and the analytics' cost were NOT measured separately.
- Only wall-clock time was measured: no CPU counters, no clock-speed log, no profiler, no memory use.

Reproduce: `.\build.ps1`, then `.\tools\run_benchmarks.ps1` (leave the machine idle; it regenerates this file from fresh runs, so the
numbers above will change slightly and the paragraphs above should be re-read against the new tables).
