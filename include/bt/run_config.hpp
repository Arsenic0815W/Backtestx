#pragma once
// The run configuration: strict JSON -> RunConfig. Unknown keys and wrong types are errors that name the key path.
// Relative paths resolve against the directory of the config file.
//
//   {
//     "name": "sma_demo",                                    optional label (default: the file name)
//     "data": {
//       "dir": "../data",                                    where <symbol>.csv files live (default "../data")
//       "files": ["RELIANCE.NS", {"symbol": "X", "path": "x.csv"}],   REQUIRED. A string means <dir>/<symbol>.csv
//       "prices": "adjusted" | "raw",                        default "adjusted"
//       "null_rows": "fail" | "skip",                        default "fail"; real Yahoo files need "skip"
//       "max_daily_move": 0.5,                               implausible-move guard for traded symbols (see dataset.hpp)
//       "exclude_dates": {"NIFTYBEES.NS": ["2019-12-19"]}    known-bad bars to drop, per symbol (each must exist)
//     },
//     "period": {"start": "2020-01-01", "end": "2023-12-31"},   evaluation window; history BEFORE start is warm-up
//     "capital": 1000000,                                    rupees
//     "costs": {...}  |  "costs_file": "costs.json",         default: the built-in delivery-equity costs (brokerage Rs 0)
//     "slippage": {"mode": "fixed_bps", "value": 5},
//     "execution": {"allow_short": false, "max_volume_participation": 0},
//     "sizing": {"allocation": 0.95},                        fraction of equity per entry (ONLY here, not in params)
//     "strategy": {"name": "sma_crossover", "params": {"fast": 20, "slow": 50}},     REQUIRED
//     "benchmark": "NIFTYBEES.NS" | {"symbol": "^NSEI", "path": "...", "max_daily_move": 0.15} | null,
//                  absent = the default benchmark NIFTYBEES.NS (a missing file is an ERROR); null = explicitly no benchmark
//     "analytics": {"risk_free_annual": 0.065, "trading_days": 252},
//     "sweep": {...}                                         only for `backtestx sweep` (see sweep.hpp)
//   }
#include <optional>
#include <string>
#include <vector>

#include "bt/analytics.hpp"
#include "bt/costs.hpp"
#include "bt/dataset.hpp"
#include "bt/json_util.hpp"

namespace bt {

struct FileSpec {
  Symbol symbol;
  std::string path;  // resolved against the config directory
};

struct BenchmarkSpec {
  enum class Mode { Default, Explicit, None };
  Mode mode = Mode::Default;
  Symbol symbol = "NIFTYBEES.NS";
  std::string path;                 // resolved
  double max_daily_move = 0.15;     // stricter than for traded symbols: an index/ETF benchmark never moves this much
};

struct RunConfig {
  std::string name = "run";
  std::string base_dir = ".";

  std::string data_dir;
  std::vector<FileSpec> files;
  PriceBasis prices = PriceBasis::Adjusted;
  NullPolicy null_rows = NullPolicy::Fail;
  double max_daily_move = 0.5;
  std::vector<std::pair<Symbol, std::vector<Date>>> exclude_dates;  // sorted by symbol

  std::optional<Date> start, end;

  double capital_rupees = 1'000'000.0;
  CostConfig costs = CostConfig::nse_delivery_defaults();
  std::string costs_source = "built-in defaults (brokerage Rs 0)";
  SlippageConfig slippage;
  bool allow_short = false;
  double max_volume_participation = 0.0;

  std::string strategy;
  nlohmann::json params = nlohmann::json::object();   // WITHOUT allocation; see `allocation`
  std::optional<double> allocation;                   // from sizing.allocation; injected into the params at run time

  BenchmarkSpec benchmark;
  AnalyticsConfig analytics;
};

// Parses `j`. If `sweep_block` is null a "sweep" key is an error (use `backtestx sweep`); otherwise the block is copied out.
// Throws ConfigError with the key path.
RunConfig parse_run_config(const nlohmann::json& j, const std::string& base_dir, const std::string& default_name,
                           nlohmann::json* sweep_block = nullptr);

// Reads and parses a config file (paths resolve against the file's directory; the default name is the file's stem).
RunConfig load_run_config(const std::string& file, nlohmann::json* sweep_block = nullptr);

nlohmann::json costs_to_json(const CostConfig& c);   // for run.json
nlohmann::json to_json(const RunConfig& c);          // the fully resolved configuration (defaults filled in)

// file name used for a symbol: "RELIANCE.NS" -> "RELIANCE.NS.csv", "^NSEI" -> "NSEI.csv" (same rule as tools/fetch_data.py)
std::string symbol_file_name(const std::string& symbol);

}  // namespace bt
