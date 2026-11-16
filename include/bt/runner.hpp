#pragma once
// Turning a RunConfig into results: load data once, window it, run the engine, analyse, write the outputs.
//
// Everything after load_run_data() is a PURE function of const inputs, so many backtests (a sweep) can run in parallel over
// the same const AlignedData.
//
// Output folder of a run (all money in integer paise):
//   equity.csv trades.csv fills.csv [benchmark.csv]   the exported rows (see export.hpp)
//   summary.json                                      EXACTLY the analytics (config + equity + trades + benchmark blocks)
//   run.json                                          everything else: resolved config, files loaded, window, warnings
#include <optional>
#include <string>
#include <vector>

#include "bt/analytics.hpp"
#include "bt/engine.hpp"
#include "bt/run_config.hpp"

namespace bt {

// Everything read from disk for one config. Immutable afterwards.
struct LoadedRun {
  std::vector<SymbolSeries> series;                // traded universe: exclusions applied, price basis applied, NOT windowed
  std::vector<LoadedFileInfo> file_info;
  std::optional<SymbolSeries> benchmark;           // same treatment
  std::optional<LoadedFileInfo> benchmark_info;
};

// Throws DatasetError (file / validation / exclusion / guard problems) with the path in the message.
LoadedRun load_run_data(const RunConfig& cfg);

// Bars dated after `last_date` are dropped (so a window's data physically cannot contain later bars).
AlignedData make_aligned(const std::vector<SymbolSeries>& series, std::optional<Date> last_date);
BenchmarkSeries make_benchmark(const SymbolSeries& b, std::optional<Date> last_date);

struct Backtest {
  EngineResult result;
  Analytics analytics;
};

// One backtest: fresh strategy from (cfg.strategy, params + sizing.allocation), engine with the config's account settings,
// evaluation window starting at `eval_start` (history before it is warm-up). Throws ConfigError for bad parameters and
// std::runtime_error if the evaluation window contains no data. Thread-safe over shared const inputs.
Backtest run_backtest(const AlignedData& data, const BenchmarkSeries* bench, const RunConfig& cfg,
                      const nlohmann::json& strategy_params, std::optional<Date> eval_start);

// Writes the run folder (creating it). `warnings` are recorded in run.json.
void write_run_outputs(const std::string& dir, const RunConfig& cfg, const LoadedRun& data, const Backtest& bt,
                       const BenchmarkSeries* bench, const nlohmann::json& params_used,
                       const std::vector<std::string>& warnings);

}  // namespace bt
