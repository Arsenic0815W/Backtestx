#include "bt/sweep.hpp"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <thread>

#include "bt/export.hpp"
#include "bt/json_util.hpp"
#include "bt/strategies.hpp"

namespace bt {
namespace {
using json = nlohmann::json;

std::string fmt2(double v) {
  char b[48];
  std::snprintf(b, sizeof b, "%.2f", v);
  return b;
}

std::string dbl(double v) {
  char b[64];
  const auto r = std::to_chars(b, b + sizeof b, v);
  return std::string(b, r.ptr);
}

// ---------------------------------------------------------------- FNV-1a
struct Fnv {
  std::uint64_t h = 1469598103934665603ULL;
  void add(std::uint64_t v) {
    for (int i = 0; i < 8; ++i) {
      h ^= (v >> (8 * i)) & 0xFFu;
      h *= 1099511628211ULL;
    }
  }
  void add_i(std::int64_t v) { add(static_cast<std::uint64_t>(v)); }
};

// ---------------------------------------------------------------- grid parsing
ParamAxis parse_axis(const std::string& name, const json& v) {
  const std::string path = "sweep.grid." + name;
  if (name.empty()) throw ConfigError("sweep.grid: empty parameter name");
  if (name == "allocation")
    throw ConfigError(path + ": allocation cannot be swept; it is set in the top-level \"sizing\" block");
  ParamAxis ax;
  ax.name = name;
  if (v.is_array()) {
    if (v.empty()) throw ConfigError(path + ": empty list of values");
    for (const json& e : v) {
      if (e.is_object() || e.is_array() || e.is_null()) throw ConfigError(path + ": values must be numbers, strings or booleans");
      ax.values.push_back(e);
    }
    return ax;
  }
  if (v.is_object()) {
    cfg::check_keys(v, {"start", "stop", "step"}, path);
    for (const char* k : {"start", "stop", "step"})
      if (!v.contains(k) || !v.at(k).is_number()) throw ConfigError(path + "." + k + ": required number");
    const json &s0 = v.at("start"), &s1 = v.at("stop"), &st = v.at("step");
    const double start = s0.get<double>(), stop = s1.get<double>(), step = st.get<double>();
    if (!(step > 0.0)) throw ConfigError(path + ".step: must be > 0");
    if (stop < start) throw ConfigError(path + ": stop is below start");
    const double span = std::floor((stop - start) / step + 1e-9);
    if (span > 1e6) throw ConfigError(path + ": range has more than a million values");
    const std::size_t count = static_cast<std::size_t>(span) + 1;
    const bool ints = s0.is_number_integer() && st.is_number_integer();
    for (std::size_t i = 0; i < count; ++i) {
      if (ints) ax.values.push_back(s0.get<std::int64_t>() + static_cast<std::int64_t>(i) * st.get<std::int64_t>());
      else ax.values.push_back(std::round((start + static_cast<double>(i) * step) * 1e10) / 1e10);  // by index, not by accumulation
    }
    return ax;
  }
  throw ConfigError(path + ": expected a list of values or {\"start\",\"stop\",\"step\"}");
}

}  // namespace

// ============================================================================ metric names
std::optional<SelectMetric> parse_metric(const std::string& n) {
  if (n == "sharpe") return SelectMetric::Sharpe;
  if (n == "sortino") return SelectMetric::Sortino;
  if (n == "cagr") return SelectMetric::Cagr;
  if (n == "total_return") return SelectMetric::TotalReturn;
  if (n == "calmar") return SelectMetric::Calmar;
  if (n == "profit_factor") return SelectMetric::ProfitFactor;
  if (n == "max_drawdown") return SelectMetric::MaxDrawdown;
  return std::nullopt;
}

const char* metric_name(SelectMetric m) {
  switch (m) {
    case SelectMetric::Sharpe: return "sharpe";
    case SelectMetric::Sortino: return "sortino";
    case SelectMetric::Cagr: return "cagr";
    case SelectMetric::TotalReturn: return "total_return";
    case SelectMetric::Calmar: return "calmar";
    case SelectMetric::ProfitFactor: return "profit_factor";
    case SelectMetric::MaxDrawdown: return "max_drawdown";
  }
  return "?";
}

SweepSpec parse_sweep_spec(const json& block) {
  cfg::check_keys(block, {"grid", "metric", "min_trades", "threads", "max_runs", "walk_forward", "warn"}, "sweep");
  SweepSpec s;
  const auto g = block.find("grid");
  if (g == block.end()) throw ConfigError("sweep.grid: required");
  cfg::expect_object(*g, "sweep.grid");
  if (g->empty()) throw ConfigError("sweep.grid: needs at least one parameter");
  for (auto it = g->begin(); it != g->end(); ++it) s.axes.push_back(parse_axis(it.key(), it.value()));  // json objects iterate sorted
  const std::string m = cfg::get_str(block, "metric", "sharpe", "sweep");
  const auto pm = parse_metric(m);
  if (!pm) throw ConfigError("sweep.metric: must be one of sharpe, sortino, cagr, total_return, calmar, profit_factor, max_drawdown; got \"" + m + "\"");
  s.metric = *pm;
  s.min_trades = cfg::get_int(block, "min_trades", 10, "sweep", 0, 1'000'000'000);
  s.threads = static_cast<std::size_t>(cfg::get_int(block, "threads", 0, "sweep", 0, 1024));
  s.max_runs = static_cast<std::size_t>(cfg::get_int(block, "max_runs", 100000, "sweep", 1, 10'000'000));
  if (const auto it = block.find("walk_forward"); it != block.end()) {
    cfg::check_keys(*it, {"in_sample_fraction", "split_date"}, "sweep.walk_forward");
    WalkForwardSpec wf;
    if (it->contains("in_sample_fraction")) {
      wf.in_sample_fraction = cfg::get_num(*it, "in_sample_fraction", 0.7, "sweep.walk_forward", 0.0, 1.0);
      if (!(*wf.in_sample_fraction > 0.0 && *wf.in_sample_fraction < 1.0))
        throw ConfigError("sweep.walk_forward.in_sample_fraction: must be strictly between 0 and 1");
    }
    if (it->contains("split_date")) {
      const std::string d = cfg::get_str(*it, "split_date", "", "sweep.walk_forward");
      const auto pd = parse_date(d);
      if (!pd) throw ConfigError("sweep.walk_forward.split_date: \"" + d + "\" is not a valid YYYY-MM-DD date");
      wf.split_date = *pd;
    }
    if (wf.in_sample_fraction.has_value() == wf.split_date.has_value())
      throw ConfigError("sweep.walk_forward: give exactly one of in_sample_fraction or split_date");
    s.walk_forward = wf;
  }
  if (const auto it = block.find("warn"); it != block.end()) {
    cfg::check_keys(*it, {"min_is_sharpe", "ratio"}, "sweep.warn");
    s.warn_min_is_sharpe = cfg::get_num(*it, "min_is_sharpe", 0.5, "sweep.warn", -100.0, 100.0);
    s.warn_ratio = cfg::get_num(*it, "ratio", 0.5, "sweep.warn", 0.0, 1.0);
  }
  return s;
}

// ============================================================================ grid
std::vector<GridPoint> expand_grid(const json& base_params, const std::vector<ParamAxis>& axes, std::size_t max_runs) {
  std::size_t total = 1;
  for (const ParamAxis& a : axes) {
    if (a.values.empty()) throw ConfigError("sweep.grid." + a.name + ": no values");
    // total * size > max_runs  <=>  total > floor(max_runs / size)   (checked before multiplying, so it cannot overflow)
    if (total > max_runs / a.values.size())
      throw ConfigError("sweep: the grid is larger than max_runs (" + std::to_string(max_runs) + "); shrink it or raise max_runs");
    total *= a.values.size();
  }
  std::vector<GridPoint> out;
  out.reserve(total);
  for (std::size_t i = 0; i < total; ++i) {
    GridPoint p;
    p.params = base_params;
    p.axis = json::object();
    std::size_t rem = i;
    for (std::size_t k = axes.size(); k-- > 0;) {   // last axis varies fastest
      const ParamAxis& a = axes[k];
      const json& v = a.values[rem % a.values.size()];
      rem /= a.values.size();
      p.params[a.name] = v;
      p.axis[a.name] = v;
    }
    out.push_back(std::move(p));
  }
  return out;
}

// ============================================================================ metrics
Metrics metrics_of(const Analytics& a) {
  Metrics m;
  m.total_return = a.equity.total_return;
  m.cagr = a.equity.cagr;
  m.volatility = a.equity.annualised_volatility;
  m.sharpe = a.equity.sharpe;
  m.sortino = a.equity.sortino;
  m.max_drawdown = a.equity.max_drawdown;
  m.calmar = a.equity.calmar;
  m.win_rate = a.trades.win_rate;
  m.profit_factor = a.trades.profit_factor;
  m.turnover_annual = a.trades.turnover_annual;
  m.fees_pct_initial_capital = a.trades.fees_pct_initial_capital;
  m.num_trades = a.trades.num_trades;
  return m;
}

std::optional<double> metric_value(const Metrics& m, SelectMetric which) {
  switch (which) {
    case SelectMetric::Sharpe: return m.sharpe;
    case SelectMetric::Sortino: return m.sortino;
    case SelectMetric::Cagr: return m.cagr;
    case SelectMetric::TotalReturn: return m.total_return;
    case SelectMetric::Calmar: return m.calmar;
    case SelectMetric::ProfitFactor: return m.profit_factor;
    case SelectMetric::MaxDrawdown: return m.max_drawdown;
  }
  return std::nullopt;
}

json to_json(const Metrics& m) {
  auto o = [](const std::optional<double>& v) { return v ? json(*v) : json(nullptr); };
  return {{"total_return", o(m.total_return)}, {"cagr", o(m.cagr)}, {"annualised_volatility", o(m.volatility)},
          {"sharpe", o(m.sharpe)}, {"sortino", o(m.sortino)}, {"max_drawdown", o(m.max_drawdown)}, {"calmar", o(m.calmar)},
          {"num_trades", m.num_trades}, {"win_rate", o(m.win_rate)}, {"profit_factor", o(m.profit_factor)},
          {"turnover_annual", o(m.turnover_annual)}, {"fees_pct_initial_capital", o(m.fees_pct_initial_capital)}};
}

std::uint64_t result_hash(const EngineResult& r) {
  Fnv f;
  for (const DailyRecord& d : r.equity_curve) {
    f.add_i(d.date); f.add_i(d.cash); f.add_i(d.positions_value); f.add_i(d.equity);
  }
  for (const Fill& x : r.fills) {
    f.add_i(x.order_id); f.add_i(static_cast<std::int64_t>(x.day_index)); f.add_i(static_cast<std::int64_t>(x.symbol));
    f.add_i(x.side == Side::Buy ? 1 : 2); f.add_i(x.qty); f.add_i(x.price); f.add_i(x.notional); f.add_i(x.fees.total());
    f.add_i(x.cash_after); f.add_i(x.position_after);
  }
  for (const Trade& t : r.trades) {
    f.add_i(static_cast<std::int64_t>(t.symbol)); f.add_i(static_cast<std::int64_t>(t.entry_day));
    f.add_i(static_cast<std::int64_t>(t.exit_day)); f.add_i(t.qty); f.add_i(t.gross_pnl); f.add_i(t.fees.total()); f.add_i(t.net_pnl);
  }
  for (const OrderEvent& e : r.events) {
    f.add_i(e.id); f.add_i(e.date); f.add_i(static_cast<std::int64_t>(e.kind)); f.add_i(static_cast<std::int64_t>(e.reason));
  }
  return f.h;
}

std::uint64_t combine_hashes(std::uint64_t is_hash, const std::optional<std::uint64_t>& oos_hash) {
  Fnv h;
  h.add(is_hash);
  if (oos_hash) h.add(*oos_hash);
  return h.h;
}

// ============================================================================ running
namespace {

void compute_row(const SweepContext& ctx, SweepRow& row) {
  try {
    const Backtest is = run_backtest(*ctx.is_data, ctx.is_bench, *ctx.cfg, row.params, ctx.is_eval_start);
    row.is = metrics_of(is.analytics);
    std::optional<std::uint64_t> oos_hash;
    if (ctx.oos_data) {
      try {
        const Backtest oos = run_backtest(*ctx.oos_data, ctx.oos_bench, *ctx.cfg, row.params, ctx.oos_eval_start);
        row.oos = metrics_of(oos.analytics);
        oos_hash = result_hash(oos.result);
      } catch (const std::exception& e) {
        row.message = std::string("out-of-sample run failed: ") + e.what();
      }
    }
    row.hash = combine_hashes(result_hash(is.result), oos_hash);
    row.status = "ok";
  } catch (const ConfigError& e) {
    row.status = "invalid";
    row.message = e.what();
  } catch (const std::exception& e) {
    row.status = "error";
    row.message = e.what();
  } catch (...) {
    row.status = "error";
    row.message = "unknown exception";
  }
}

}  // namespace

SweepRun run_sweep(const SweepContext& ctx, const std::vector<GridPoint>& grid, std::size_t threads) {
  SweepRun out;
  out.rows.resize(grid.size());
  std::vector<std::size_t> todo;
  std::string first_invalid;
  for (std::size_t i = 0; i < grid.size(); ++i) {
    SweepRow& r = out.rows[i];
    r.id = i;
    r.axis = grid[i].axis;
    r.params = grid[i].params;
    try {  // cheap, sequential: reject bad parameter combinations before any data is touched
      json p = r.params;
      if (ctx.cfg->allocation) p["allocation"] = *ctx.cfg->allocation;
      make_strategy(ctx.cfg->strategy, p);
      todo.push_back(i);
    } catch (const ConfigError& e) {
      r.status = "invalid";
      r.message = e.what();
      if (first_invalid.empty()) first_invalid = e.what();
    }
  }
  if (todo.empty()) throw ConfigError("sweep: every grid point is invalid (first: " + first_invalid + ")");

  std::size_t n = threads ? threads : std::max<std::size_t>(1, std::thread::hardware_concurrency());
  n = std::max<std::size_t>(1, std::min(n, todo.size()));
  out.threads_used = n;

  std::atomic<std::size_t> next{0};
  auto worker = [&]() {
    for (;;) {
      const std::size_t k = next.fetch_add(1);
      if (k >= todo.size()) return;
      compute_row(ctx, out.rows[todo[k]]);   // each task writes only its own row (indexed by grid position)
    }
  };
  if (n == 1) {
    worker();
  } else {
    std::vector<std::thread> pool;
    pool.reserve(n);
    for (std::size_t t = 0; t < n; ++t) pool.emplace_back(worker);
    for (std::thread& t : pool) t.join();
  }
  return out;
}

std::optional<std::size_t> select_best(const std::vector<SweepRow>& rows, SelectMetric metric, std::int64_t min_trades) {
  std::optional<std::size_t> best;
  double best_v = 0.0;
  for (std::size_t i = 0; i < rows.size(); ++i) {
    const SweepRow& r = rows[i];
    if (r.status != "ok" || !r.is) continue;
    if (r.is->num_trades < min_trades) continue;
    const auto v = metric_value(*r.is, metric);
    if (!v) continue;                    // an undefined metric is never selected
    if (!best || *v > best_v) {          // strict: ties keep the earliest (lowest id)
      best = i;
      best_v = *v;
    }
  }
  return best;
}

// ============================================================================ windows & warnings
Windows split_window(const std::vector<Date>& calendar, std::optional<Date> start, std::optional<Date> end, const WalkForwardSpec& wf) {
  std::vector<Date> win;
  for (Date d : calendar)
    if ((!start || d >= *start) && (!end || d <= *end)) win.push_back(d);
  if (win.size() < 2) throw std::runtime_error("walk_forward: the evaluation window has fewer than 2 days");
  std::size_t k;  // number of in-sample days; win[k] is the first out-of-sample day
  if (wf.in_sample_fraction) {
    // + 1e-9: so that e.g. 0.7 * 10 (which may be 6.999...) still gives 7 in-sample days
    k = static_cast<std::size_t>(std::floor(*wf.in_sample_fraction * static_cast<double>(win.size()) + 1e-9));
    k = std::min(std::max<std::size_t>(k, 1), win.size() - 1);
  } else {
    const auto it = std::lower_bound(win.begin(), win.end(), *wf.split_date);
    k = static_cast<std::size_t>(it - win.begin());
    if (k == 0) throw std::runtime_error("walk_forward: split_date " + format_date(*wf.split_date) + " is at or before the first evaluation day: no in-sample data");
    if (k >= win.size()) throw std::runtime_error("walk_forward: split_date " + format_date(*wf.split_date) + " is after the last evaluation day: no out-of-sample data");
  }
  Windows w;
  w.first_date = win.front();
  w.is_last = win[k - 1];
  w.oos_first = win[k];
  w.last_date = win.back();
  w.is_days = k;
  w.oos_days = win.size() - k;
  return w;
}

std::vector<std::string> overfit_warnings(const Metrics& is, const Metrics& oos, std::size_t oos_days, double min_is_sharpe, double ratio) {
  std::vector<std::string> w;
  if (is.sharpe && *is.sharpe >= min_is_sharpe) {
    if (!oos.sharpe) {
      w.push_back("WARNING (rule 1, in-sample Sharpe high, out-of-sample Sharpe collapsed): in-sample Sharpe is " + fmt2(*is.sharpe) +
                  " but the out-of-sample Sharpe is undefined (no variance or no trades)");
    } else if (*oos.sharpe < ratio * *is.sharpe) {
      w.push_back("WARNING (rule 1, in-sample Sharpe high, out-of-sample Sharpe collapsed): in-sample Sharpe " + fmt2(*is.sharpe) +
                  " >> out-of-sample Sharpe " + fmt2(*oos.sharpe) + " (below " + fmt2(ratio) + " x in-sample): the selected parameters look overfitted");
    }
  }
  if (oos.num_trades == 0) w.push_back("WARNING: the selected parameters made no trades out of sample");
  if (oos_days < 60)
    w.push_back("WARNING: the out-of-sample window is only " + std::to_string(oos_days) + " days; its metrics are very unreliable");
  return w;
}

std::vector<std::string> overfit_warnings(const Metrics& is, const Metrics& oos, std::size_t oos_days, double min_is_sharpe, double ratio,
                                          const OverfitContext& ctx) {
  std::vector<std::string> w = overfit_warnings(is, oos, oos_days, min_is_sharpe, ratio);
  const std::string m = metric_name(ctx.metric);
  // Rule 2: the selected combination ranks in the bottom half out of sample (strictly below the median rank, (n + 1) / 2).
  if (ctx.oos_rank && ctx.oos_of) {
    if (*ctx.oos_of >= 2 && 2 * *ctx.oos_rank > *ctx.oos_of + 1)
      w.push_back("WARNING (rule 2, bottom half out of sample): the selected parameters rank " + std::to_string(*ctx.oos_rank) + " of " +
                  std::to_string(*ctx.oos_of) + " combinations on out-of-sample " + m + ", below the median rank; choosing on the in-sample window did not pick a robust combination");
  } else {
    w.push_back("WARNING (rule 2, bottom half out of sample): the out-of-sample " + m + " of the selected parameters is undefined, so its rank among the combinations cannot be computed");
  }
  // Rule 3: positive in-sample, negative out of sample (not meaningful for max_drawdown, which is never positive).
  if (ctx.metric != SelectMetric::MaxDrawdown) {
    const auto vi = metric_value(is, ctx.metric), vo = metric_value(oos, ctx.metric);
    if (vi && vo && *vi > 0.0 && *vo < 0.0)
      w.push_back("WARNING (rule 3, sign flip): " + m + " is positive in-sample (" + fmt2(*vi) + ") but negative out-of-sample (" + fmt2(*vo) + ")");
  }
  return w;
}

// ============================================================================ output
namespace {

std::string csv_field(const std::string& s) {
  if (s.find_first_of(",\"\n\r") == std::string::npos) return s;
  std::string o = "\"";
  for (char c : s) {
    if (c == '"') o += '"';
    o += c;
  }
  return o + "\"";
}

std::string axis_cell(const json& v) { return v.is_string() ? csv_field(v.get<std::string>()) : v.dump(); }
std::string opt_cell(const std::optional<double>& v) { return v ? dbl(*v) : std::string(); }

void metrics_cells(std::ostream& o, const std::optional<Metrics>& m) {
  const char* sep = ",";
  if (!m) {
    for (int i = 0; i < 12; ++i) o << sep;
    return;
  }
  o << sep << opt_cell(m->total_return) << sep << opt_cell(m->cagr) << sep << opt_cell(m->volatility) << sep << opt_cell(m->sharpe)
    << sep << opt_cell(m->sortino) << sep << opt_cell(m->max_drawdown) << sep << opt_cell(m->calmar) << sep << m->num_trades
    << sep << opt_cell(m->win_rate) << sep << opt_cell(m->profit_factor) << sep << opt_cell(m->turnover_annual) << sep
    << opt_cell(m->fees_pct_initial_capital);
}

const char* kMetricCols[] = {"total_return", "cagr", "annualised_volatility", "sharpe", "sortino", "max_drawdown", "calmar",
                             "num_trades", "win_rate", "profit_factor", "turnover_annual", "fees_pct_initial_capital"};

void write_sweep_csv(std::ostream& o, const SweepSpec& spec, const std::vector<SweepRow>& rows, std::optional<std::size_t> best, bool has_oos) {
  // is_* = the selection window (the whole evaluation window when there is no split). oos_* = out-of-sample, DIAGNOSTIC only.
  o << "run_id";
  for (const ParamAxis& a : spec.axes) o << ',' << csv_field(a.name);
  o << ",status,message";
  for (const char* c : kMetricCols) o << ",is_" << c;
  if (has_oos)
    for (const char* c : kMetricCols) o << ",oos_" << c;
  o << ",run_hash,selected\n";
  for (const SweepRow& r : rows) {
    o << r.id;
    for (const ParamAxis& a : spec.axes) o << ',' << axis_cell(r.axis.at(a.name));
    o << ',' << r.status << ',' << csv_field(r.message);
    metrics_cells(o, r.is);
    if (has_oos) metrics_cells(o, r.oos);
    char hb[24];
    std::snprintf(hb, sizeof hb, "%016llx", static_cast<unsigned long long>(r.hash));
    o << ',' << (r.status == "ok" ? hb : "") << ',' << (best && *best == r.id ? 1 : 0) << '\n';
  }
}

json window_json(Date first, Date last, std::size_t days) {
  return {{"start", format_date(first)}, {"end", format_date(last)}, {"days", days}};
}

}  // namespace

SweepOutcome execute_sweep(const RunConfig& cfg, const SweepSpec& spec, const LoadedRun& data, std::size_t threads_override,
                           const std::string& out_dir) {
  SweepOutcome out;
  if (!data.benchmark && cfg.benchmark.mode != BenchmarkSpec::Mode::None) throw std::runtime_error("benchmark requested but not loaded");

  // ---- windows. The in-sample data is PHYSICALLY cut: it contains no bar after the split. ----
  const AlignedData full = make_aligned(data.series, cfg.end);
  std::optional<BenchmarkSeries> full_bench;
  if (data.benchmark) full_bench = make_benchmark(*data.benchmark, cfg.end);

  std::optional<Windows> win;
  std::optional<AlignedData> is_only;
  std::optional<BenchmarkSeries> is_bench_only;
  SweepContext ctx;
  ctx.cfg = &cfg;
  ctx.is_eval_start = cfg.start;
  if (spec.walk_forward) {
    win = split_window(full.dates(), cfg.start, cfg.end, *spec.walk_forward);
    is_only = make_aligned(data.series, win->is_last);
    if (data.benchmark) is_bench_only = make_benchmark(*data.benchmark, win->is_last);
    ctx.is_data = &*is_only;
    ctx.is_bench = is_bench_only ? &*is_bench_only : nullptr;
    ctx.oos_data = &full;
    ctx.oos_bench = full_bench ? &*full_bench : nullptr;
    ctx.oos_eval_start = win->oos_first;
  } else {
    ctx.is_data = &full;
    ctx.is_bench = full_bench ? &*full_bench : nullptr;
  }

  // ---- grid, runs, selection ----
  const std::vector<GridPoint> grid = expand_grid(cfg.params, spec.axes, spec.max_runs);
  SweepRun run = run_sweep(ctx, grid, threads_override ? threads_override : spec.threads);
  out.rows = std::move(run.rows);
  out.threads_used = run.threads_used;

  std::size_t n_ok = 0, n_invalid = 0, n_error = 0;
  for (const SweepRow& r : out.rows) {
    if (r.status == "ok") ++n_ok;
    else if (r.status == "invalid") ++n_invalid;
    else ++n_error;
  }
  if (n_ok == 0) {
    std::string first;
    for (const SweepRow& r : out.rows)
      if (r.status == "error") { first = r.message; break; }
    throw std::runtime_error("sweep: no grid point ran successfully (first error: " + first + ")");
  }
  out.best = select_best(out.rows, spec.metric, spec.min_trades);
  std::size_t n_eligible = 0;
  for (const SweepRow& r : out.rows)
    if (r.status == "ok" && r.is && r.is->num_trades >= spec.min_trades && metric_value(*r.is, spec.metric)) ++n_eligible;

  // ---- warnings ----
  if (!out.best)
    out.warnings.push_back(std::string("WARNING: no combination qualifies for selection (needs >= ") + std::to_string(spec.min_trades) +
                           " closed trades and a defined " + metric_name(spec.metric) + ")");
  if (!spec.walk_forward)
    out.warnings.push_back("WARNING: no out-of-sample split configured: the best result is selected on the same data it is reported on, "
                           "so it is optimistically biased (best of " + std::to_string(out.rows.size()) + ")");
  std::optional<std::size_t> oos_rank, oos_of;
  if (out.best && spec.walk_forward) {
    const SweepRow& b = out.rows[*out.best];
    if (b.is && b.oos) {
      if (const auto mine = metric_value(*b.oos, spec.metric)) {
        std::size_t better = 0, defined = 0;
        for (const SweepRow& r : out.rows) {
          if (r.status != "ok" || !r.oos) continue;
          const auto v = metric_value(*r.oos, spec.metric);
          if (!v) continue;
          ++defined;
          if (*v > *mine) ++better;
        }
        oos_rank = better + 1;
        oos_of = defined;
      }
      OverfitContext octx;
      octx.metric = spec.metric;
      octx.oos_rank = oos_rank;
      octx.oos_of = oos_of;
      for (const std::string& w : overfit_warnings(*b.is, *b.oos, win->oos_days, spec.warn_min_is_sharpe, spec.warn_ratio, octx)) out.warnings.push_back(w);
    } else {
      out.warnings.push_back("WARNING: the selected combination has no out-of-sample result" + (b.message.empty() ? std::string() : (": " + b.message)));
    }
  }

  // ---- files ----
  std::filesystem::create_directories(out_dir);
  {
    std::ofstream f(out_dir + "/sweep.csv", std::ios::binary);
    if (!f) throw std::runtime_error("cannot write " + out_dir + "/sweep.csv");
    write_sweep_csv(f, spec, out.rows, out.best, spec.walk_forward.has_value());
  }

  json axes = json::object();
  for (const ParamAxis& a : spec.axes) axes[a.name] = a.values;
  json best = nullptr;
  if (out.best) {
    const SweepRow& b = out.rows[*out.best];
    best = {{"run_id", b.id}, {"params", b.axis}, {"in_sample", to_json(*b.is)},
            {"out_of_sample", b.oos ? to_json(*b.oos) : json(nullptr)},
            {"out_of_sample_rank", oos_rank ? json({{"rank", *oos_rank}, {"of", *oos_of}}) : json(nullptr)}};
  }
  json windows = {{"mode", spec.walk_forward ? "single_split" : "none"}};
  if (win) {
    windows["in_sample"] = window_json(win->first_date, win->is_last, win->is_days);
    windows["out_of_sample"] = window_json(win->oos_first, win->last_date, win->oos_days);
  }
  out.summary = {
      {"name", cfg.name},
      {"strategy", cfg.strategy},
      {"selection_metric", metric_name(spec.metric)},
      {"selection_rule", "highest value of the metric on the in-sample window; undefined values never win; needs min_trades closed trades; ties go to the lowest run_id"},
      {"min_trades", spec.min_trades},
      {"grid", {{"axes", axes}, {"n_combinations", out.rows.size()}}},
      {"counts", {{"ok", n_ok}, {"invalid", n_invalid}, {"error", n_error}, {"eligible_for_selection", n_eligible}}},
      {"threads_used", out.threads_used},
      {"windows", windows},
      {"best", best},
      {"warnings", out.warnings},
      {"notes",
       json::array({"The best of N in-sample results is biased upward; N = n_combinations above.",
                    "A single in-sample / out-of-sample split is not a rolling walk-forward.",
                    "The oos_* columns of sweep.csv exist for every combination as a diagnostic; they are never used for selection."})}};
  {
    std::ofstream f(out_dir + "/sweep_summary.json", std::ios::binary);
    if (!f) throw std::runtime_error("cannot write " + out_dir + "/sweep_summary.json");
    f << out.summary.dump(2) << '\n';
  }

  // ---- full outputs for the selected parameters: the out-of-sample run when there is a split, else the whole window ----
  if (out.best) {
    const SweepRow& b = out.rows[*out.best];
    RunConfig bc = cfg;
    const AlignedData* d = ctx.is_data;
    const BenchmarkSeries* bs = ctx.is_bench;
    std::optional<Date> start = ctx.is_eval_start;
    if (spec.walk_forward) {
      d = ctx.oos_data;
      bs = ctx.oos_bench;
      start = ctx.oos_eval_start;
      bc.start = start;
      bc.name = cfg.name + " (best parameters, out-of-sample window)";
    } else {
      bc.name = cfg.name + " (best parameters)";
    }
    const Backtest bt = run_backtest(*d, bs, bc, b.params, start);
    json used = b.params;
    if (cfg.allocation) used["allocation"] = *cfg.allocation;
    write_run_outputs(out_dir + "/best", bc, data, bt, bs, used, out.warnings);
  }
  return out;
}

}  // namespace bt
