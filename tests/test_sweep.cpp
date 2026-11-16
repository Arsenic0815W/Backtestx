#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <random>
#include <set>
#include <sstream>
#include <thread>

#include "bt/strategies.hpp"
#include "bt/sweep.hpp"
#include "bt/synthetic.hpp"
#include "doctest.h"
#include "test_util.hpp"

using namespace bt;
using nlohmann::json;
namespace fs = std::filesystem;

namespace {

bool has(const std::string& s, const char* part) { return s.find(part) != std::string::npos; }

std::string spec_err(const std::string& text) {
  try {
    parse_sweep_spec(json::parse(text));
    return "";
  } catch (const ConfigError& e) {
    return e.what();
  }
}

SweepRow mkrow(std::size_t id, const char* status, std::optional<double> sharpe, std::int64_t trades) {
  SweepRow r;
  r.id = id;
  r.status = status;
  Metrics m;
  m.sharpe = sharpe;
  m.num_trades = trades;
  r.is = m;
  return r;
}

// ---- an in-memory fixture (no files): two synthetic symbols, SMA on A ----
struct Fixture {
  RunConfig cfg;
  std::vector<SymbolSeries> series;
  AlignedData full;
  BenchmarkSeries bench;
};

std::vector<Bar> gbm(std::uint64_t seed, int bars, double price, const char* start = "2021-01-04") {
  SynthParams p;
  p.start = parse_date(start).value();
  p.num_bars = bars;
  p.seed = seed;
  p.start_price = price;
  p.annual_vol = 0.3;
  return generate_gbm(p);
}

Fixture make_fixture(std::uint64_t seed = 1, int bars = 600) {
  Fixture f;
  f.cfg = parse_run_config(json::parse(R"({"data": {"files": ["A", "B"]}, "benchmark": null,
                                           "sizing": {"allocation": 0.95},
                                           "strategy": {"name": "sma_crossover", "params": {"symbol": "A"}}})"),
                           "/p", "fx");
  f.series = {{"A", gbm(seed, bars, 100)}, {"B", gbm(seed + 100, bars, 500)}};
  f.full = make_aligned(f.series, std::nullopt);
  f.bench = benchmark_from_bars("IDX", gbm(seed + 200, bars, 10000));
  return f;
}

SweepSpec spec_of(const char* text) { return parse_sweep_spec(json::parse(text)); }

std::string dump(const std::optional<Metrics>& m) { return m ? to_json(*m).dump() : "none"; }

}  // namespace

// ============================================================ spec & grid
TEST_CASE("sweep spec: defaults, ranges, and the order of the grid") {
  const SweepSpec s = spec_of(R"({"grid": {"slow": {"start": 30, "stop": 50, "step": 10}, "fast": [5, 10]}})");
  CHECK(s.metric == SelectMetric::Sharpe);
  CHECK(s.min_trades == 10);
  CHECK(s.threads == 0);
  CHECK(s.max_runs == 100000);
  CHECK_FALSE(s.walk_forward.has_value());
  REQUIRE(s.axes.size() == 2);
  CHECK(s.axes[0].name == "fast");                       // alphabetical, whatever the file order
  CHECK(s.axes[1].name == "slow");
  CHECK(s.axes[1].values == std::vector<json>{30, 40, 50});
  CHECK(s.axes[1].values[0].is_number_integer());       // integer start/step => JSON integers (the factory rejects 30.0)

  const auto grid = expand_grid(json::parse(R"({"symbol": "A"})"), s.axes, 1000);
  REQUIRE(grid.size() == 6);
  // axes alphabetical (fast, slow); the LAST axis (slow) varies fastest
  const int expect[6][2] = {{5, 30}, {5, 40}, {5, 50}, {10, 30}, {10, 40}, {10, 50}};
  for (int i = 0; i < 6; ++i) {
    CHECK(grid[static_cast<std::size_t>(i)].axis["fast"] == expect[i][0]);
    CHECK(grid[static_cast<std::size_t>(i)].axis["slow"] == expect[i][1]);
    CHECK(grid[static_cast<std::size_t>(i)].params["symbol"] == "A");    // base params carried along
    CHECK(grid[static_cast<std::size_t>(i)].axis.size() == 2);
  }
}

TEST_CASE("sweep spec: float ranges are computed by index, not by accumulation") {
  const SweepSpec s = spec_of(R"({"grid": {"x": {"start": 0.1, "stop": 0.5, "step": 0.1}}})");
  // Repeated addition would give 0.30000000000000004 and might drop the endpoint; by-index + rounding gives clean values.
  CHECK(s.axes[0].values == std::vector<json>{0.1, 0.2, 0.3, 0.4, 0.5});
  const SweepSpec t = spec_of(R"({"grid": {"x": {"start": 0, "stop": 1, "step": 0.25}}})");
  CHECK(t.axes[0].values.size() == 5);
  CHECK(t.axes[0].values[0].is_number_float());          // step is a float => floats (0.0 stays a float)
  const SweepSpec one = spec_of(R"({"grid": {"x": {"start": 7, "stop": 7, "step": 1}}})");
  CHECK(one.axes[0].values == std::vector<json>{7});
}

TEST_CASE("sweep spec: strict validation") {
  CHECK(has(spec_err(R"({})"), "sweep.grid"));
  CHECK(has(spec_err(R"({"grid": {}})"), "at least one"));
  CHECK(has(spec_err(R"({"grid": {"x": []}})"), "sweep.grid.x"));
  CHECK(has(spec_err(R"({"grid": {"x": 5}})"), "sweep.grid.x"));
  CHECK(has(spec_err(R"({"grid": {"x": [[1]]}})"), "sweep.grid.x"));
  CHECK(has(spec_err(R"({"grid": {"x": {"start": 1, "stop": 5}}})"), "step"));
  CHECK(has(spec_err(R"({"grid": {"x": {"start": 1, "stop": 5, "step": 0}}})"), "step"));
  CHECK(has(spec_err(R"({"grid": {"x": {"start": 5, "stop": 1, "step": 1}}})"), "below start"));
  CHECK(has(spec_err(R"({"grid": {"x": {"start": 1, "stop": 5, "step": 1, "extra": 1}}})"), "extra"));
  CHECK(has(spec_err(R"({"grid": {"allocation": [0.5, 1]}})"), "sizing"));
  CHECK(has(spec_err(R"({"grid": {"x": [1]}, "metric": "luck"})"), "sweep.metric"));
  CHECK(has(spec_err(R"({"grid": {"x": [1]}, "min_trades": -1})"), "sweep.min_trades"));
  CHECK(has(spec_err(R"({"grid": {"x": [1]}, "min_trades": 2.5})"), "sweep.min_trades"));
  CHECK(has(spec_err(R"({"grid": {"x": [1]}, "threads": -2})"), "sweep.threads"));
  CHECK(has(spec_err(R"({"grid": {"x": [1]}, "max_runs": 0})"), "sweep.max_runs"));
  CHECK(has(spec_err(R"({"grid": {"x": [1]}, "bogus": 1})"), "sweep.bogus"));
  // walk_forward: exactly one of the two, fraction strictly inside (0,1), valid date
  CHECK(has(spec_err(R"({"grid": {"x": [1]}, "walk_forward": {}})"), "exactly one"));
  CHECK(has(spec_err(R"({"grid": {"x": [1]}, "walk_forward": {"in_sample_fraction": 0.7, "split_date": "2022-01-01"}})"), "exactly one"));
  CHECK(has(spec_err(R"({"grid": {"x": [1]}, "walk_forward": {"in_sample_fraction": 1.0}})"), "in_sample_fraction"));
  CHECK(has(spec_err(R"({"grid": {"x": [1]}, "walk_forward": {"in_sample_fraction": 0}})"), "in_sample_fraction"));
  CHECK(has(spec_err(R"({"grid": {"x": [1]}, "walk_forward": {"split_date": "2022-02-30"}})"), "split_date"));
  CHECK(has(spec_err(R"({"grid": {"x": [1]}, "walk_forward": {"cut": 1}})"), "walk_forward.cut"));
  CHECK(has(spec_err(R"({"grid": {"x": [1]}, "warn": {"ratio": 2}})"), "sweep.warn.ratio"));
  // every metric name is accepted
  for (const char* m : {"sharpe", "sortino", "cagr", "total_return", "calmar", "profit_factor", "max_drawdown"}) {
    const SweepSpec s = spec_of((std::string(R"({"grid": {"x": [1]}, "metric": ")") + m + "\"}").c_str());
    CHECK(std::string(metric_name(s.metric)) == m);
  }
}

TEST_CASE("sweep grid: a grid bigger than max_runs is refused (without overflowing)") {
  const SweepSpec s = spec_of(R"({"grid": {"a": {"start": 1, "stop": 100, "step": 1}, "b": {"start": 1, "stop": 100, "step": 1}}, "max_runs": 5000})");
  CHECK_THROWS_AS(expand_grid(json::object(), s.axes, s.max_runs), ConfigError);
  CHECK(expand_grid(json::object(), s.axes, 10000).size() == 10000);                // exactly at the limit is fine
  CHECK_THROWS_AS(expand_grid(json::object(), s.axes, 9999), ConfigError);          // one below is not
  std::vector<ParamAxis> huge(6, ParamAxis{"x", std::vector<json>(1000, 1)});
  for (std::size_t i = 0; i < huge.size(); ++i) huge[i].name = "p" + std::to_string(i);
  CHECK_THROWS_AS(expand_grid(json::object(), huge, 100000), ConfigError);          // 1000^6 would overflow naive arithmetic
}

// ============================================================ selection
TEST_CASE("selection: highest metric wins; undefined never wins; min_trades guards; ties go to the lowest id") {
  std::vector<SweepRow> rows = {mkrow(0, "ok", 0.5, 20), mkrow(1, "ok", 1.8, 20), mkrow(2, "ok", 1.2, 20), mkrow(3, "ok", 1.8, 20)};
  CHECK(select_best(rows, SelectMetric::Sharpe, 10).value() == 1);       // tie 1.8: lowest id
  rows[1].is->sharpe = std::nullopt;                                     // undefined => never selected
  CHECK(select_best(rows, SelectMetric::Sharpe, 10).value() == 3);
  rows[3].is->num_trades = 9;                                            // below min_trades
  CHECK(select_best(rows, SelectMetric::Sharpe, 10).value() == 2);
  CHECK(select_best(rows, SelectMetric::Sharpe, 9).value() == 3);        // 9 >= 9: eligible again
  rows[2].status = "invalid";
  rows[3].status = "error";
  CHECK(select_best(rows, SelectMetric::Sharpe, 10).value() == 0);
  rows[0].is->num_trades = 3;
  CHECK_FALSE(select_best(rows, SelectMetric::Sharpe, 10).has_value());  // nothing eligible
  CHECK(select_best({}, SelectMetric::Sharpe, 0) == std::nullopt);
}

TEST_CASE("selection: the direction of 'better' per metric - max_drawdown closer to zero wins") {
  std::vector<SweepRow> rows = {mkrow(0, "ok", 0, 20), mkrow(1, "ok", 0, 20), mkrow(2, "ok", 0, 20)};
  rows[0].is->max_drawdown = -0.30;
  rows[1].is->max_drawdown = -0.10;
  rows[2].is->max_drawdown = -0.20;
  CHECK(select_best(rows, SelectMetric::MaxDrawdown, 0).value() == 1);   // -10 % is the SMALLEST drawdown
  rows[0].is->cagr = 0.05; rows[1].is->cagr = 0.02; rows[2].is->cagr = 0.11;
  CHECK(select_best(rows, SelectMetric::Cagr, 0).value() == 2);
  rows[0].is->profit_factor = 2.0; rows[1].is->profit_factor = 0.5; rows[2].is->profit_factor = std::nullopt;
  CHECK(select_best(rows, SelectMetric::ProfitFactor, 0).value() == 0);
  // a metric that is undefined everywhere selects nothing
  CHECK_FALSE(select_best(rows, SelectMetric::Calmar, 0).has_value());
}

TEST_CASE("selection reads ONLY the in-sample metrics: scrambling the out-of-sample diagnostics cannot change it") {
  std::vector<SweepRow> rows = {mkrow(0, "ok", 0.5, 20), mkrow(1, "ok", 1.8, 20), mkrow(2, "ok", 1.2, 20)};
  const auto before = select_best(rows, SelectMetric::Sharpe, 10);
  std::mt19937_64 rng(5);
  for (int trial = 0; trial < 20; ++trial) {
    for (SweepRow& r : rows) {
      Metrics m;
      m.sharpe = static_cast<double>(rng() % 2000) / 100.0 - 10.0;
      m.num_trades = static_cast<std::int64_t>(rng() % 50);
      m.max_drawdown = -static_cast<double>(rng() % 100) / 100.0;
      r.oos = m;
    }
    CHECK(select_best(rows, SelectMetric::Sharpe, 10) == before);
  }
}

// ============================================================ windows & warnings
TEST_CASE("walk-forward split: every day lands in exactly one window; boundaries") {
  std::vector<Date> cal;
  for (Date d = 100; d < 110; ++d) cal.push_back(d);                     // 10 days: 100..109
  WalkForwardSpec f70;
  f70.in_sample_fraction = 0.7;
  Windows w = split_window(cal, std::nullopt, std::nullopt, f70);
  CHECK(w.is_days == 7);
  CHECK(w.oos_days == 3);
  CHECK(w.is_last == 106);
  CHECK(w.oos_first == 107);                                             // the day after is_last: no gap, no overlap
  CHECK(w.is_days + w.oos_days == cal.size());
  CHECK(w.first_date == 100);
  CHECK(w.last_date == 109);

  WalkForwardSpec tiny, huge;
  tiny.in_sample_fraction = 0.01;
  huge.in_sample_fraction = 0.999;
  CHECK(split_window(cal, std::nullopt, std::nullopt, tiny).is_days == 1);          // never an empty side
  CHECK(split_window(cal, std::nullopt, std::nullopt, huge).oos_days == 1);

  // split_date: the date itself starts the out-of-sample window
  WalkForwardSpec on;
  on.split_date = 105;
  w = split_window(cal, std::nullopt, std::nullopt, on);
  CHECK(w.oos_first == 105);
  CHECK(w.is_last == 104);
  // a split date that is not a trading day: out-of-sample starts at the NEXT day
  const std::vector<Date> gappy = {100, 101, 102, 103, 104, 106, 107, 108, 109};
  w = split_window(gappy, std::nullopt, std::nullopt, on);
  CHECK(w.oos_first == 106);
  CHECK(w.is_last == 104);
  // before the first / after the last day: one side would be empty
  WalkForwardSpec before, after;
  before.split_date = 100;
  after.split_date = 500;
  CHECK_THROWS_AS(split_window(cal, std::nullopt, std::nullopt, before), std::runtime_error);
  CHECK_THROWS_AS(split_window(cal, std::nullopt, std::nullopt, after), std::runtime_error);
  // the evaluation window [start, end] is honoured
  w = split_window(cal, Date{102}, Date{107}, f70);                      // days 102..107 = 6 days; floor(0.7*6)=4
  CHECK(w.first_date == 102);
  CHECK(w.last_date == 107);
  CHECK(w.is_days == 4);
  CHECK(w.is_last == 105);
  CHECK(w.oos_first == 106);
  CHECK_THROWS_AS(split_window(cal, Date{108}, Date{108}, f70), std::runtime_error);   // a single day cannot be split
}

TEST_CASE("overfitting warning rule (a heuristic): both sides, undefined, no trades, tiny window") {
  auto M = [](std::optional<double> sharpe, std::int64_t trades) {
    Metrics m;
    m.sharpe = sharpe;
    m.num_trades = trades;
    return m;
  };
  // in-sample 1.5, threshold ratio 0.5 => warn when out-of-sample Sharpe < 0.75
  CHECK(overfit_warnings(M(1.5, 20), M(0.40, 10), 200, 0.5, 0.5).size() == 1);
  CHECK(overfit_warnings(M(1.5, 20), M(0.74, 10), 200, 0.5, 0.5).size() == 1);
  CHECK(overfit_warnings(M(1.5, 20), M(0.75, 10), 200, 0.5, 0.5).empty());          // exactly at the threshold: no warning
  CHECK(overfit_warnings(M(1.5, 20), M(0.90, 10), 200, 0.5, 0.5).empty());
  CHECK(overfit_warnings(M(1.5, 20), M(-0.3, 10), 200, 0.5, 0.5).size() == 1);      // negative out of sample
  const auto undef = overfit_warnings(M(1.5, 20), M(std::nullopt, 10), 200, 0.5, 0.5);
  REQUIRE(undef.size() == 1);
  CHECK(has(undef[0].c_str(), "undefined"));
  // in-sample Sharpe below the floor: the degradation rule is silent (nothing impressive to lose) ...
  CHECK(overfit_warnings(M(0.3, 20), M(-1.0, 10), 200, 0.5, 0.5).empty());
  CHECK(overfit_warnings(M(0.5, 20), M(0.1, 10), 200, 0.5, 0.5).size() == 1);       // ... but exactly at the floor it applies
  // no out-of-sample trades
  const auto nt = overfit_warnings(M(0.3, 20), M(std::nullopt, 0), 200, 0.5, 0.5);
  REQUIRE(nt.size() == 1);
  CHECK(has(nt[0].c_str(), "no trades"));
  // tiny window
  CHECK(overfit_warnings(M(0.3, 20), M(0.3, 5), 59, 0.5, 0.5).size() == 1);
  CHECK(overfit_warnings(M(0.3, 20), M(0.3, 5), 60, 0.5, 0.5).empty());
  // the message states the numbers
  const auto msg = overfit_warnings(M(1.5, 20), M(0.40, 10), 200, 0.5, 0.5);
  CHECK(has(msg[0].c_str(), "1.50"));
  CHECK(has(msg[0].c_str(), "0.40"));
}

// ============================================================ threads: the results do not depend on the thread count
namespace {

struct RefRow {
  std::string status;
  std::string message;
  std::string is, oos;
  std::uint64_t hash;
};

// A plain sequential loop that does NOT use run_sweep or its thread pool.
std::vector<RefRow> sequential_reference(const SweepContext& ctx, const std::vector<GridPoint>& grid) {
  std::vector<RefRow> out;
  for (const GridPoint& g : grid) {
    RefRow r;
    try {
      json p = g.params;
      if (ctx.cfg->allocation) p["allocation"] = *ctx.cfg->allocation;
      make_strategy(ctx.cfg->strategy, p);
    } catch (const ConfigError& e) {
      r.status = "invalid";
      r.message = e.what();
      r.hash = 0;
      out.push_back(r);
      continue;
    }
    const Backtest is = run_backtest(*ctx.is_data, ctx.is_bench, *ctx.cfg, g.params, ctx.is_eval_start);
    r.is = dump(metrics_of(is.analytics));
    std::optional<std::uint64_t> oos_hash;
    if (ctx.oos_data) {
      const Backtest o = run_backtest(*ctx.oos_data, ctx.oos_bench, *ctx.cfg, g.params, ctx.oos_eval_start);
      r.oos = dump(metrics_of(o.analytics));
      oos_hash = result_hash(o.result);
    } else {
      r.oos = "none";
    }
    r.status = "ok";
    r.hash = combine_hashes(result_hash(is.result), oos_hash);
    out.push_back(r);
  }
  return out;
}

void expect_same(const std::vector<SweepRow>& rows, const std::vector<RefRow>& ref) {
  REQUIRE(rows.size() == ref.size());
  for (std::size_t i = 0; i < rows.size(); ++i) {
    CAPTURE(i);
    CHECK(rows[i].id == i);                          // rows are in grid order, whatever finished first
    CHECK(rows[i].status == ref[i].status);
    CHECK(rows[i].message == ref[i].message);
    CHECK(dump(rows[i].is) == (ref[i].status == "ok" ? ref[i].is : "none"));
    CHECK(dump(rows[i].oos) == (ref[i].status == "ok" ? ref[i].oos : "none"));
    CHECK(rows[i].hash == ref[i].hash);
  }
}

}  // namespace

TEST_CASE("threads: sweep results are identical for 1, 2, 4, N threads and equal to a sequential loop (no TSan on MinGW: this is the evidence)") {
  const Fixture f = make_fixture(3, 700);
  // 4 x 4 grid including invalid points (fast >= slow) that must stay in place as 'invalid' rows
  const SweepSpec spec = spec_of(R"({"grid": {"fast": [5, 8, 25, 40], "slow": [20, 30, 45, 60]}})");
  const auto grid = expand_grid(f.cfg.params, spec.axes, spec.max_runs);
  REQUIRE(grid.size() == 16);

  SweepContext ctx;
  ctx.cfg = &f.cfg;
  ctx.is_data = &f.full;
  ctx.is_bench = &f.bench;
  const auto ref = sequential_reference(ctx, grid);
  std::size_t invalid = 0;
  for (const RefRow& r : ref) invalid += r.status == "invalid";
  REQUIRE(invalid >= 3);                                 // (25,20) (40,20) (40,30) (40,... ) - the grid really has bad points

  const std::size_t n_all = std::max<std::size_t>(8, std::thread::hardware_concurrency());
  for (std::size_t threads : {std::size_t{1}, std::size_t{2}, std::size_t{4}, n_all, std::size_t{64}, std::size_t{0}}) {
    for (int repeat = 0; repeat < 3; ++repeat) {         // scheduling differs between repeats
      CAPTURE(threads);
      CAPTURE(repeat);
      const SweepRun run = run_sweep(ctx, grid, threads);
      expect_same(run.rows, ref);
      CHECK(run.threads_used >= 1);
      CHECK(run.threads_used <= grid.size() - invalid);   // never more threads than runnable points
      if (threads == 1) CHECK(run.threads_used == 1);
    }
  }
}

TEST_CASE("threads: the same holds with an out-of-sample window and a benchmark") {
  const Fixture f = make_fixture(7, 700);
  const SweepSpec spec = spec_of(R"({"grid": {"fast": [4, 6, 9], "slow": [20, 35, 50]}})");
  const auto grid = expand_grid(f.cfg.params, spec.axes, spec.max_runs);
  WalkForwardSpec wf;
  wf.in_sample_fraction = 0.6;
  const Windows w = split_window(f.full.dates(), std::nullopt, std::nullopt, wf);
  const AlignedData is_only = make_aligned(f.series, w.is_last);

  SweepContext ctx;
  ctx.cfg = &f.cfg;
  ctx.is_data = &is_only;
  ctx.is_bench = &f.bench;
  ctx.oos_data = &f.full;
  ctx.oos_bench = &f.bench;
  ctx.oos_eval_start = w.oos_first;
  const auto ref = sequential_reference(ctx, grid);
  for (std::size_t threads : {std::size_t{1}, std::size_t{3}, std::size_t{16}}) {
    CAPTURE(threads);
    expect_same(run_sweep(ctx, grid, threads).rows, ref);
  }
  // the out-of-sample column really is filled and differs from the in-sample one
  const SweepRun run = run_sweep(ctx, grid, 2);
  bool differ = false;
  for (const SweepRow& r : run.rows) {
    REQUIRE(r.oos.has_value());
    differ |= dump(r.is) != dump(r.oos);
  }
  CHECK(differ);
}

TEST_CASE("sweep: every grid point invalid -> a clear error; bad points are rows, not crashes") {
  const Fixture f = make_fixture();
  SweepContext ctx;
  ctx.cfg = &f.cfg;
  ctx.is_data = &f.full;
  const auto all_bad = expand_grid(f.cfg.params, spec_of(R"({"grid": {"fast": [50, 60], "slow": [10, 20]}})").axes, 100);
  try {
    run_sweep(ctx, all_bad, 2);
    FAIL("should have thrown");
  } catch (const ConfigError& e) {
    CHECK(has(e.what(), "every grid point is invalid"));
    CHECK(has(e.what(), "fast"));
  }
  // an unknown parameter name is also reported per row (the factory rejects it)
  const auto typo = expand_grid(f.cfg.params, spec_of(R"({"grid": {"fats": [5], "slow": [20]}})").axes, 100);
  CHECK_THROWS_AS(run_sweep(ctx, typo, 1), ConfigError);
  // a mixed grid: bad rows carry the message, good rows run
  const auto mixed = expand_grid(f.cfg.params, spec_of(R"({"grid": {"fast": [5, 50], "slow": [20]}})").axes, 100);
  const SweepRun run = run_sweep(ctx, mixed, 4);
  REQUIRE(run.rows.size() == 2);
  CHECK(run.rows[0].status == "ok");
  CHECK(run.rows[1].status == "invalid");
  CHECK(has(run.rows[1].message.c_str(), "fast"));
  CHECK_FALSE(run.rows[1].is.has_value());
  CHECK(run.threads_used == 1);                         // only one runnable point => one thread
}

// ============================================================ the whole sweep: windows, blindness, outputs
namespace {

std::vector<Bar> garbage_bars(Date from, std::size_t n, std::uint64_t seed) {
  std::mt19937_64 rng(seed);
  auto U = [&] { return static_cast<double>(rng() >> 11) / 9007199254740992.0; };
  auto cents = [](double x) { return std::max(0.01, std::round(x * 100.0) / 100.0); };
  std::vector<Bar> out;
  Date d = from;
  for (std::size_t i = 0; i < n; ++i) {
    const double base = std::exp(U() * std::log(5000.0));
    Bar b;
    b.date = d;
    b.open = cents(base * (0.9 + 0.2 * U()));
    b.close = cents(base * (0.9 + 0.2 * U()));
    b.high = std::max({cents(std::max(b.open, b.close) * (1 + 0.1 * U())), b.open, b.close});
    b.low = std::min({cents(std::min(b.open, b.close) * (1 - 0.1 * U())), b.open, b.close});
    b.adj_close = b.close;
    b.volume = 1000;
    out.push_back(b);
    d += 1 + static_cast<Date>(rng() % 3);
  }
  return out;
}

std::string slurp(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

struct TmpDir {
  fs::path path;
  explicit TmpDir(const char* name) : path(fs::temp_directory_path() / (std::string("bt_test_") + name)) {
    fs::remove_all(path);
    fs::create_directories(path);
  }
  ~TmpDir() { fs::remove_all(path); }
};

LoadedRun loaded_from(const std::vector<SymbolSeries>& s) {
  LoadedRun lr;
  lr.series = s;
  return lr;
}

}  // namespace

TEST_CASE("out-of-sample BLINDNESS: replacing every bar after the split by garbage cannot change the selection or any in-sample number") {
  const Fixture f = make_fixture(11, 700);
  const Date split = f.series[0].bars[450].date;
  RunConfig cfg = f.cfg;
  cfg.start = f.series[0].bars[150].date;     // 150 bars of warm-up before the evaluation window
  SweepSpec sp = spec_of(R"({"grid": {"fast": [4, 6, 9, 14], "slow": [20, 30, 45, 60]}, "min_trades": 3})");
  WalkForwardSpec wf;
  wf.split_date = split;
  sp.walk_forward = wf;
  sp.threads = 2;

  // dataset 2: identical before the split, garbage on and after it (different prices, dates and holes)
  std::vector<SymbolSeries> garbled;
  std::uint64_t k = 0;
  for (const SymbolSeries& s : f.series) {
    SymbolSeries g{s.symbol, {}};
    std::size_t later = 0;
    for (const Bar& b : s.bars) {
      if (b.date < split) g.bars.push_back(b);
      else ++later;
    }
    for (const Bar& b : garbage_bars(split, later, 900 + (++k))) g.bars.push_back(b);
    garbled.push_back(std::move(g));
  }

  const TmpDir t1("blind1"), t2("blind2");
  const SweepOutcome a = execute_sweep(cfg, sp, loaded_from(f.series), 0, t1.path.string());
  const SweepOutcome b = execute_sweep(cfg, sp, loaded_from(garbled), 0, t2.path.string());

  REQUIRE(a.rows.size() == b.rows.size());
  REQUIRE(a.best.has_value());
  REQUIRE(b.best.has_value());
  CHECK(*a.best == *b.best);                                      // the same parameters are selected
  CHECK(a.rows[*a.best].axis == b.rows[*b.best].axis);
  bool oos_differs = false;
  for (std::size_t i = 0; i < a.rows.size(); ++i) {
    CAPTURE(i);
    CHECK(a.rows[i].status == b.rows[i].status);
    CHECK(dump(a.rows[i].is) == dump(b.rows[i].is));              // EVERY in-sample number is identical
    oos_differs |= dump(a.rows[i].oos) != dump(b.rows[i].oos);
  }
  CHECK(oos_differs);                                             // positive control: the garbage does change the out-of-sample side
  // the in-sample columns of sweep.csv are byte-identical row by row; only the oos_* columns may differ
  auto cols = [](const std::string& csv, bool want_oos) {
    std::vector<std::string> out;
    std::istringstream in(csv);
    std::string line;
    std::vector<std::string> header;
    bool first = true;
    while (std::getline(in, line)) {
      std::vector<std::string> cells;
      std::stringstream ls(line);
      std::string c;
      while (std::getline(ls, c, ',')) cells.push_back(c);
      if (first) {
        header = cells;
        first = false;
        continue;
      }
      std::string keep;
      for (std::size_t i = 0; i < cells.size() && i < header.size(); ++i) {
        const bool is_oos = header[i].rfind("oos_", 0) == 0 || header[i] == "run_hash";
        if (is_oos == want_oos) keep += cells[i] + "|";
      }
      out.push_back(keep);
    }
    return out;
  };
  const std::string csv1 = slurp(t1.path / "sweep.csv"), csv2 = slurp(t2.path / "sweep.csv");
  CHECK(cols(csv1, false) == cols(csv2, false));
  CHECK(cols(csv1, true) != cols(csv2, true));
}

TEST_CASE("execute_sweep: out-of-sample starts with fresh capital, uses earlier history only as warm-up, and the files are written") {
  const Fixture f = make_fixture(5, 700);
  RunConfig cfg = f.cfg;
  cfg.start = f.series[0].bars[100].date;
  SweepSpec sp = spec_of(R"({"grid": {"fast": [5, 10], "slow": [30, 50]}, "min_trades": 1})");
  WalkForwardSpec wf;
  wf.in_sample_fraction = 0.6;
  sp.walk_forward = wf;
  const TmpDir t("oosfresh");
  const SweepOutcome r = execute_sweep(cfg, sp, loaded_from(f.series), 3, t.path.string());
  REQUIRE(r.best.has_value());
  CHECK(r.threads_used >= 1);
  CHECK(r.threads_used <= 3);                                     // the explicit thread count is honoured as an upper bound

  // files
  for (const char* name : {"sweep.csv", "sweep_summary.json"}) CHECK(fs::exists(t.path / name));
  for (const char* name : {"equity.csv", "trades.csv", "fills.csv", "summary.json", "run.json"}) CHECK(fs::exists(t.path / "best" / name));
  CHECK_FALSE(fs::exists(t.path / "best" / "benchmark.csv"));     // this fixture has no benchmark

  // the best/ run is the OUT-OF-SAMPLE run: it starts on oos_first with exactly the initial capital
  const json summary = json::parse(slurp(t.path / "sweep_summary.json"));
  const std::string oos_start = summary["windows"]["out_of_sample"]["start"];
  const std::string is_end = summary["windows"]["in_sample"]["end"];
  CHECK(summary["windows"]["mode"] == "single_split");
  CHECK(is_end < oos_start);                                      // ISO dates compare lexicographically
  const std::string eq = slurp(t.path / "best" / "equity.csv");
  const std::size_t nl = eq.find('\n') + 1;
  CHECK(eq.substr(nl, 10) == oos_start);
  const json bsum = json::parse(slurp(t.path / "best" / "summary.json"));
  CHECK(bsum["equity"]["start_equity_paise"] == 100'000'000);     // fresh Rs 10,00,000, not carried over from in-sample
  CHECK(bsum["equity"]["start_date"] == oos_start);
  // sweep.csv: one line per combination + header; the oos columns exist; exactly one row is marked selected
  const std::string csv = slurp(t.path / "sweep.csv");
  CHECK(std::count(csv.begin(), csv.end(), '\n') == 5);
  CHECK(has(csv, "oos_sharpe"));
  CHECK(has(csv, "is_sharpe"));
  std::istringstream in(csv);
  std::string line;
  std::getline(in, line);
  int selected = 0;
  while (std::getline(in, line)) selected += !line.empty() && line.back() == '1';
  CHECK(selected == 1);
  // the notes tell the reader what the numbers are
  CHECK(summary["notes"].size() == 3);
  CHECK(summary["grid"]["n_combinations"] == 4);
}

TEST_CASE("execute_sweep without a split warns that the best result is selected and reported on the same data") {
  const Fixture f = make_fixture(5, 500);
  RunConfig cfg = f.cfg;
  const SweepSpec sp = spec_of(R"({"grid": {"fast": [5, 10], "slow": [30, 50]}, "min_trades": 1})");
  const TmpDir t("nosplit");
  const SweepOutcome r = execute_sweep(cfg, sp, loaded_from(f.series), 1, t.path.string());
  REQUIRE_FALSE(r.warnings.empty());
  CHECK(has(r.warnings[0].c_str(), "no out-of-sample split"));
  CHECK(has(r.warnings[0].c_str(), "best of 4"));
  const std::string csv = slurp(t.path / "sweep.csv");
  CHECK_FALSE(has(csv, "oos_"));                                  // no oos columns without a split
  CHECK(json::parse(slurp(t.path / "sweep_summary.json"))["windows"]["mode"] == "none");
}

TEST_CASE("execute_sweep: min_trades guard - when nothing qualifies there is no 'best' and a warning says why") {
  const Fixture f = make_fixture(5, 400);
  const SweepSpec sp = spec_of(R"({"grid": {"fast": [5, 10], "slow": [30, 50]}, "min_trades": 100000})");
  const TmpDir t("noqual");
  const SweepOutcome r = execute_sweep(f.cfg, sp, loaded_from(f.series), 1, t.path.string());
  CHECK_FALSE(r.best.has_value());
  bool warned = false;
  for (const std::string& w : r.warnings) warned |= has(w.c_str(), "no combination qualifies");
  CHECK(warned);
  CHECK(fs::exists(t.path / "sweep.csv"));                        // the table is still written
  CHECK_FALSE(fs::exists(t.path / "best"));
  CHECK(json::parse(slurp(t.path / "sweep_summary.json"))["best"].is_null());
}

// ------------------------------------------------------------------------------------------------ overfit rules 2 and 3
namespace {
Metrics sharpe_metrics(std::optional<double> sharpe, std::int64_t trades = 10) {
  Metrics m;
  m.sharpe = sharpe;
  m.num_trades = trades;
  return m;
}
OverfitContext ctx_of(std::optional<std::size_t> rank, std::optional<std::size_t> of, SelectMetric metric = SelectMetric::Sharpe) {
  OverfitContext c;
  c.metric = metric;
  c.oos_rank = rank;
  c.oos_of = of;
  return c;
}
}  // namespace

TEST_CASE("overfit rules: the RELIANCE pattern (in-sample Sharpe 0.222 below the rule-1 floor, out-of-sample -0.850, rank 15 of 15) now warns") {
  const Metrics is = sharpe_metrics(0.222, 15), oos = sharpe_metrics(-0.850, 8);
  // the original rule alone is silent: in-sample Sharpe 0.222 < 0.5
  CHECK(overfit_warnings(is, oos, 1097, 0.5, 0.5).empty());
  const auto w = overfit_warnings(is, oos, 1097, 0.5, 0.5, ctx_of(15, 15));
  REQUIRE(w.size() == 2);
  CHECK(has(w[0], "rule 2"));
  CHECK(has(w[0], "15 of 15"));
  CHECK(has(w[1], "rule 3"));
  CHECK(has(w[1], "0.22"));
  CHECK(has(w[1], "-0.85"));
  for (const std::string& x : w) CHECK_FALSE(has(x, "rule 1"));
}

TEST_CASE("overfit rule 2 (bottom half): fires strictly below the median rank, never for a single combination, and says so when the rank is undefined") {
  const Metrics is = sharpe_metrics(0.3), oos = sharpe_metrics(0.2);     // positive both sides: only rule 2 can fire
  auto fires = [&](std::size_t rank, std::size_t of) {
    const auto w = overfit_warnings(is, oos, 200, 0.5, 0.5, ctx_of(rank, of));
    for (const std::string& x : w)
      if (has(x, "rule 2")) return true;
    return false;
  };
  CHECK_FALSE(fires(1, 1));
  CHECK_FALSE(fires(1, 2));
  CHECK(fires(2, 2));
  CHECK_FALSE(fires(2, 3));      // the median of 3
  CHECK(fires(3, 3));
  CHECK_FALSE(fires(8, 15));     // the median of 15
  CHECK(fires(9, 15));
  CHECK_FALSE(fires(8, 16));
  CHECK(fires(9, 16));
  const auto undef = overfit_warnings(is, oos, 200, 0.5, 0.5, ctx_of(std::nullopt, std::nullopt));
  REQUIRE(undef.size() == 1);
  CHECK(has(undef[0], "rule 2"));
  CHECK(has(undef[0], "undefined"));
}

TEST_CASE("overfit rule 3 (sign flip): positive in-sample and negative out of sample only; not applied to max_drawdown; uses the selection metric") {
  auto fires3 = [&](const Metrics& is, const Metrics& oos, SelectMetric m) {
    for (const std::string& x : overfit_warnings(is, oos, 200, 0.5, 0.5, ctx_of(1, 16, m)))
      if (has(x, "rule 3")) return true;
    return false;
  };
  CHECK(fires3(sharpe_metrics(0.01), sharpe_metrics(-0.01), SelectMetric::Sharpe));
  CHECK_FALSE(fires3(sharpe_metrics(0.0), sharpe_metrics(-1.0), SelectMetric::Sharpe));      // not positive in-sample
  CHECK_FALSE(fires3(sharpe_metrics(1.0), sharpe_metrics(0.0), SelectMetric::Sharpe));       // zero is not negative
  CHECK_FALSE(fires3(sharpe_metrics(-1.0), sharpe_metrics(-2.0), SelectMetric::Sharpe));     // negative both sides: no flip
  CHECK_FALSE(fires3(sharpe_metrics(0.5), sharpe_metrics(std::nullopt), SelectMetric::Sharpe));
  Metrics a, b;
  a.cagr = 0.10;
  b.cagr = -0.05;
  a.sharpe = -1.0;  // Sharpe is irrelevant when the selection metric is CAGR
  b.sharpe = -1.0;
  CHECK(fires3(a, b, SelectMetric::Cagr));
  Metrics d1, d2;
  d1.max_drawdown = -0.1;
  d2.max_drawdown = -0.3;
  CHECK_FALSE(fires3(d1, d2, SelectMetric::MaxDrawdown));
}

TEST_CASE("overfit rules, end to end on synthetic data: a weak in-sample result that fails out of sample is flagged by rules 2 and 3, not rule 1") {
  // A noisy cyclic series that drifts up with period-130 cycles for the first 70 % of the bars, then drifts down with fast period-22 cycles
  // (a regime change). The noise is an integer hash, so the series is identical on every platform.
  std::vector<Bar> bars;
  Date d = parse_date("2015-01-05").value();
  while (bars.size() < 1500) {
    if (!is_weekend(d)) {
      const int i = static_cast<int>(bars.size());
      const double pi = 3.14159265358979323846;
      double price;
      const int split = 1050;
      if (i < split) {
        price = 100 * std::exp(0.0006 * i) * (1 + 0.03 * std::sin(2 * pi * i / 130));
      } else {
        const int j = i - split;
        const double base = 100 * std::exp(0.0006 * split) * (1 + 0.03 * std::sin(2 * pi * split / 130));
        price = base * std::exp(-0.0009 * j) * (1 + 0.06 * std::sin(2 * pi * j / 22));
      }
      const double noise = static_cast<double>(((static_cast<std::uint64_t>(i) * 2654435761ULL) % 4294967296ULL) % 2001) / 1000.0 - 1.0;
      price *= 1 + 0.022 * noise;
      price = std::round(price * 100.0) / 100.0;
      Bar b;
      b.date = d;
      b.open = b.high = b.low = b.close = b.adj_close = price;
      b.volume = 1'000'000;
      bars.push_back(b);
    }
    ++d;
  }
  std::vector<SymbolSeries> series = {{"PATT", bars}};
  RunConfig cfg;
  cfg.name = "pattern";
  cfg.strategy = "sma_crossover";
  cfg.params = {{"symbol", "PATT"}};
  cfg.allocation = 0.95;
  cfg.benchmark.mode = BenchmarkSpec::Mode::None;
  cfg.start = parse_date("2015-06-01").value();
  // min_is_sharpe 1.0 puts the in-sample Sharpe below rule 1's floor, exactly the RELIANCE situation (0.222 vs 0.5)
  const SweepSpec sp = spec_of(R"({"grid": {"fast": [5, 10, 15, 20], "slow": [30, 50, 70, 90]}, "min_trades": 3,
                                   "walk_forward": {"in_sample_fraction": 0.7}, "warn": {"min_is_sharpe": 1.0, "ratio": 0.5}})");
  const TmpDir t("pattern");
  const SweepOutcome r = execute_sweep(cfg, sp, loaded_from(series), 0, t.path.string());
  REQUIRE(r.best.has_value());
  const SweepRow& b = r.rows[*r.best];
  REQUIRE(b.is.has_value());
  REQUIRE(b.oos.has_value());
  CHECK(*b.is->sharpe > 0.0);
  CHECK(*b.is->sharpe < 1.0);          // below rule 1's floor
  CHECK(*b.oos->sharpe < 0.0);
  const json s = json::parse(slurp(t.path / "sweep_summary.json"));
  const auto rank = s["best"]["out_of_sample_rank"];
  REQUIRE(rank.is_object());
  CHECK(2 * rank["rank"].get<std::size_t>() > rank["of"].get<std::size_t>() + 1);   // bottom half
  bool rule1 = false, rule2 = false, rule3 = false;
  for (const std::string& w : r.warnings) {
    rule1 |= has(w, "rule 1");
    rule2 |= has(w, "rule 2");
    rule3 |= has(w, "rule 3");
  }
  CHECK_FALSE(rule1);
  CHECK(rule2);
  CHECK(rule3);
  // and they reach sweep_summary.json
  int in_summary = 0;
  for (const auto& w : s["warnings"])
    if (has(w.get<std::string>(), "rule 2") || has(w.get<std::string>(), "rule 3")) ++in_summary;
  CHECK(in_summary == 2);
}
