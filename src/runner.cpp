#include "bt/runner.hpp"

#include <algorithm>
#include <fstream>
#include <map>
#include <stdexcept>

#include "bt/export.hpp"
#include "bt/strategies.hpp"
#include "bt/version.hpp"

namespace bt {

LoadedRun load_run_data(const RunConfig& cfg) {
  DatasetOptions opt;
  opt.basis = cfg.prices;
  opt.load.null_policy = cfg.null_rows;
  auto excluded_for = [&](const Symbol& s) {
    for (const auto& [sym, dates] : cfg.exclude_dates)
      if (sym == s) return dates;
    return std::vector<Date>{};
  };

  LoadedRun out;
  std::vector<DataFile> files;
  for (const FileSpec& f : cfg.files) files.emplace_back(f.symbol, f.path, excluded_for(f.symbol), cfg.max_daily_move);
  out.series = load_dataset(files, opt, &out.file_info);

  if (cfg.benchmark.mode != BenchmarkSpec::Mode::None) {
    std::vector<LoadedFileInfo> info;
    const DataFile bf(cfg.benchmark.symbol, cfg.benchmark.path, excluded_for(cfg.benchmark.symbol), cfg.benchmark.max_daily_move);
    try {
      out.benchmark = load_dataset({bf}, opt, &info)[0];
    } catch (const DatasetError& e) {
      throw DatasetError(std::string("benchmark ") + cfg.benchmark.symbol + ": " + e.what() +
                         (cfg.benchmark.mode == BenchmarkSpec::Mode::Default
                              ? "  [NIFTYBEES.NS is the DEFAULT benchmark; set \"benchmark\": null for none or name another symbol]"
                              : ""));
    }
    out.benchmark_info = info.at(0);
  }
  return out;
}

AlignedData make_aligned(const std::vector<SymbolSeries>& series, std::optional<Date> last_date) {
  std::vector<SymbolSeries> cut;
  cut.reserve(series.size());
  for (const SymbolSeries& s : series) {
    SymbolSeries c{s.symbol, {}};
    c.bars.reserve(s.bars.size());
    for (const Bar& b : s.bars) {
      if (last_date && b.date > *last_date) break;  // bars are strictly increasing
      c.bars.push_back(b);
    }
    cut.push_back(std::move(c));
  }
  return align_calendar(std::move(cut));
}

BenchmarkSeries make_benchmark(const SymbolSeries& b, std::optional<Date> last_date) {
  std::vector<Bar> bars;
  for (const Bar& x : b.bars) {
    if (last_date && x.date > *last_date) break;
    bars.push_back(x);
  }
  return benchmark_from_bars(b.symbol, bars);
}

Backtest run_backtest(const AlignedData& data, const BenchmarkSeries* bench, const RunConfig& cfg,
                      const nlohmann::json& strategy_params, std::optional<Date> eval_start) {
  EngineConfig ec;
  ec.initial_cash = to_paise(cfg.capital_rupees);
  ec.costs = cfg.costs;
  ec.slippage = cfg.slippage;
  ec.allow_short = cfg.allow_short;
  ec.max_volume_participation = cfg.max_volume_participation;
  ec.eval_start = eval_start;

  nlohmann::json p = strategy_params;  // a private copy: never mutate JSON shared between threads
  if (cfg.allocation) p["allocation"] = *cfg.allocation;
  auto strat = make_strategy(cfg.strategy, p);

  Backtest b;
  b.result = Engine(data, ec).run(*strat);
  if (b.result.equity_curve.empty())
    throw std::runtime_error("the evaluation window contains no data" +
                             (eval_start ? " (start " + format_date(*eval_start) + ")" : std::string()));
  b.analytics = analyze(b.result.equity_curve, b.result.trades, b.result.fills, cfg.analytics, bench);
  return b;
}

namespace {
using json = nlohmann::json;

json file_json(const LoadedFileInfo& f) {
  json ex = json::array();
  for (Date d : f.excluded) ex.push_back(format_date(d));
  return {{"symbol", f.symbol},
          {"path", f.path},
          {"bars", f.bars},
          {"first_date", format_date(f.first)},
          {"last_date", format_date(f.last)},
          {"skipped_null_rows", f.skipped_null_rows},
          {"excluded_dates", ex},
          {"largest_close_to_close_move", f.largest_move},
          {"largest_move_date", format_date(f.largest_move_date)}};
}
}  // namespace

void write_run_outputs(const std::string& dir, const RunConfig& cfg, const LoadedRun& data, const Backtest& bt,
                       const BenchmarkSeries* bench, const nlohmann::json& params_used,
                       const std::vector<std::string>& warnings) {
  export_run(dir, bt.result, bt.analytics, bench);

  std::map<std::string, int> rejects;
  int expired = 0, cancelled = 0;
  for (const OrderEvent& e : bt.result.events) {
    if (e.kind == OrderEventKind::Rejected) ++rejects[to_string(e.reason)];
    else if (e.kind == OrderEventKind::Expired) ++expired;
    else ++cancelled;
  }
  json files = json::array();
  for (const LoadedFileInfo& f : data.file_info) files.push_back(file_json(f));

  json run = to_json(cfg);
  run["strategy"]["params"] = params_used;  // exactly what the strategy was built with
  json doc = {
      {"tool", "backtestx"},
      {"version", version()},
      {"config", run},
      {"data", {{"traded", files}, {"benchmark", data.benchmark_info ? file_json(*data.benchmark_info) : json(nullptr)}}},
      {"window",
       {{"requested_start", cfg.start ? json(format_date(*cfg.start)) : json(nullptr)},
        {"requested_end", cfg.end ? json(format_date(*cfg.end)) : json(nullptr)},
        {"first_evaluation_date", format_date(bt.result.equity_curve.front().date)},
        {"last_evaluation_date", format_date(bt.result.equity_curve.back().date)},
        {"evaluation_days", bt.result.equity_curve.size()},
        {"warmup_days", bt.result.warmup_days},
        {"warmup_orders_discarded", bt.result.warmup_orders_discarded}}},
      {"orders",
       {{"fills", bt.result.fills.size()},
        {"rejected_by_reason", rejects},
        {"expired", expired},
        {"cancelled", cancelled},
        {"pending_at_end", bt.result.orders_pending_at_end}}},
      {"warnings", warnings}};
  std::ofstream f(dir + "/run.json", std::ios::binary);
  if (!f) throw std::runtime_error("cannot write " + dir + "/run.json");
  f << doc.dump(2) << '\n';
}

}  // namespace bt
