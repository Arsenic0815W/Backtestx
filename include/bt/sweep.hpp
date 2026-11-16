#pragma once
// Parameter sweeps and the overfitting guards around them.
//
// Config: a run config plus a "sweep" block
//   "sweep": {
//     "grid":   {"fast": [5, 10, 20], "slow": {"start": 30, "stop": 100, "step": 10}},   strategy params to vary (>= 1 axis)
//     "metric": "sharpe",              selection metric: sharpe|sortino|cagr|total_return|calmar|profit_factor|max_drawdown
//                                      (HIGHER is better for all; max_drawdown is <= 0, so closer to 0 wins)
//     "min_trades": 10,                combinations with fewer closed trades in the selection window are never selected
//     "threads": 0,                    0 = hardware_concurrency; the CLI --threads overrides
//     "max_runs": 100000,              refuse bigger grids
//     "walk_forward": {"in_sample_fraction": 0.7}  |  {"split_date": "2022-01-01"},        optional
//     "warn": {"min_is_sharpe": 0.5, "ratio": 0.5}                                          heuristic, see overfit_warnings()
//   }
//
// Grid order is deterministic: axes in alphabetical order of their names, the LAST axis varying fastest. Range axes are
// computed by index (start + i*step), never by accumulation; an axis whose start and step are integers yields JSON integers
// (the strategy factory rejects 14.0 for an integer parameter). "allocation" cannot be swept (it lives in "sizing").
//
// What this is NOT: a single in-sample / out-of-sample split is not a rolling walk-forward. And whatever the split, the best of
// N in-sample results is biased upward - that is why the out-of-sample figures exist.
//
// Threading: every grid point is an independent run over the same const AlignedData with its own Strategy and its own copy of
// the parameters; results are stored by grid index (never by completion order), so the output is identical for any thread
// count (tests compare 1, 2, 4 and N threads against a plain sequential loop). There is no ThreadSanitizer on MinGW; those
// tests are the evidence.
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "bt/runner.hpp"

namespace bt {

struct ParamAxis {
  std::string name;
  std::vector<nlohmann::json> values;
};

enum class SelectMetric { Sharpe, Sortino, Cagr, TotalReturn, Calmar, ProfitFactor, MaxDrawdown };
std::optional<SelectMetric> parse_metric(const std::string& name);
const char* metric_name(SelectMetric m);

struct WalkForwardSpec {
  std::optional<double> in_sample_fraction;  // (0,1)
  std::optional<Date> split_date;            // first day of the out-of-sample window (or the next bar after it)
};

struct SweepSpec {
  std::vector<ParamAxis> axes;               // alphabetical by name
  SelectMetric metric = SelectMetric::Sharpe;
  std::int64_t min_trades = 10;
  std::size_t threads = 0;
  std::size_t max_runs = 100000;
  std::optional<WalkForwardSpec> walk_forward;
  double warn_min_is_sharpe = 0.5;
  double warn_ratio = 0.5;
};

// Throws ConfigError with "sweep...." key paths.
SweepSpec parse_sweep_spec(const nlohmann::json& block);

struct GridPoint {
  nlohmann::json params;  // base strategy params with the axis values applied
  nlohmann::json axis;    // only the varied parameters (for sweep.csv)
};
// Throws ConfigError if the grid is larger than max_runs.
std::vector<GridPoint> expand_grid(const nlohmann::json& base_params, const std::vector<ParamAxis>& axes, std::size_t max_runs);

// The numbers kept per run (a compact view of Analytics).
struct Metrics {
  std::optional<double> total_return, cagr, volatility, sharpe, sortino, max_drawdown, calmar, win_rate, profit_factor,
      turnover_annual, fees_pct_initial_capital;
  std::int64_t num_trades = 0;
};
Metrics metrics_of(const Analytics& a);
std::optional<double> metric_value(const Metrics& m, SelectMetric which);
nlohmann::json to_json(const Metrics& m);

struct SweepRow {
  std::size_t id = 0;
  nlohmann::json axis;    // varied parameters
  nlohmann::json params;  // full strategy params
  std::string status = "pending";   // ok | invalid (bad parameters) | error
  std::string message;
  std::optional<Metrics> is;        // selection window (the whole evaluation window when there is no split)
  std::optional<Metrics> oos;       // out-of-sample window; DIAGNOSTIC only, never used for selection
  std::uint64_t hash = 0;           // FNV-1a over every equity record, fill and trade of the run(s)
};

struct SweepContext {
  const RunConfig* cfg = nullptr;
  const AlignedData* is_data = nullptr;
  const BenchmarkSeries* is_bench = nullptr;
  std::optional<Date> is_eval_start;
  const AlignedData* oos_data = nullptr;       // null = no out-of-sample run
  const BenchmarkSeries* oos_bench = nullptr;
  std::optional<Date> oos_eval_start;
};

struct SweepRun {
  std::vector<SweepRow> rows;   // in grid order
  std::size_t threads_used = 1;
};
// Runs every grid point. threads == 0 -> hardware_concurrency (at least 1); never more threads than runnable points.
// Invalid parameter combinations become rows with status "invalid"; if EVERY point is invalid this throws ConfigError.
SweepRun run_sweep(const SweepContext& ctx, const std::vector<GridPoint>& grid, std::size_t threads);

// Index of the best row, or nullopt. Eligible: status ok, selection-window metrics present, the metric defined (an undefined
// metric is never selected), num_trades >= min_trades. Ties go to the lowest id. Uses ONLY the in-sample metrics.
std::optional<std::size_t> select_best(const std::vector<SweepRow>& rows, SelectMetric metric, std::int64_t min_trades);

// A hash of one engine result (equity curve, fills, trades, events): used to prove runs are bit-identical across thread counts.
std::uint64_t result_hash(const EngineResult& r);
// The hash stored in a sweep row: the in-sample result hash, folded with the out-of-sample one when there is one.
std::uint64_t combine_hashes(std::uint64_t is_hash, const std::optional<std::uint64_t>& oos_hash);

struct Windows {
  Date first_date = 0, is_last = 0, oos_first = 0, last_date = 0;
  std::size_t is_days = 0, oos_days = 0;
};
// Splits the calendar days in [start, end] into an in-sample part and an out-of-sample part; every day is in exactly one.
// Throws ConfigError / std::runtime_error if either part would be empty.
Windows split_window(const std::vector<Date>& calendar, std::optional<Date> start, std::optional<Date> end,
                     const WalkForwardSpec& wf);

// HEURISTIC warnings (thresholds from the spec): in-sample Sharpe >= min_is_sharpe but out-of-sample Sharpe below
// ratio * in-sample (including negative or undefined), no out-of-sample trades, or a tiny out-of-sample window (< 60 days).
std::vector<std::string> overfit_warnings(const Metrics& is, const Metrics& oos, std::size_t oos_days, double min_is_sharpe,
                                          double ratio);

// What the extra overfit rules need beyond the two Metrics: the selection metric and where the selected combination ranks among all
// combinations on that metric out of sample (rank 1 = best; nullopt = the selected combination's out-of-sample value is undefined).
struct OverfitContext {
  SelectMetric metric = SelectMetric::Sharpe;
  std::optional<std::size_t> oos_rank, oos_of;
};
// The three heuristic rules plus the two plain checks; every message says which rule fired:
//   rule 1  in-sample Sharpe >= min_is_sharpe but out-of-sample Sharpe < ratio x in-sample (or undefined)   [the 5-argument overload]
//   rule 2  the selected parameters rank in the bottom half out of sample on the selection metric (rank strictly below the median)
//   rule 3  the selection metric is positive in-sample and negative out of sample (not applied to max_drawdown)
// Rules 2 and 3 exist because rule 1 is silent when the in-sample result was unimpressive (the RELIANCE example: in-sample Sharpe 0.222).
std::vector<std::string> overfit_warnings(const Metrics& is, const Metrics& oos, std::size_t oos_days, double min_is_sharpe, double ratio,
                                          const OverfitContext& ctx);

struct SweepOutcome {
  std::vector<SweepRow> rows;
  std::optional<std::size_t> best;
  std::vector<std::string> warnings;
  nlohmann::json summary;
  std::size_t threads_used = 1;
};
// The whole `backtestx sweep`: windows, grid, runs, selection, files (sweep.csv, sweep_summary.json, best/ ...).
SweepOutcome execute_sweep(const RunConfig& cfg, const SweepSpec& spec, const LoadedRun& data, std::size_t threads_override,
                           const std::string& out_dir);

}  // namespace bt
