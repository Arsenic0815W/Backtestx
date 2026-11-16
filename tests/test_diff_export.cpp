// Writes a set of diverse finished runs to ${build}/diff/<scenario>/ (equity.csv, trades.csv, fills.csv, [benchmark.csv],
// summary.json). build.ps1 then runs tools/ref_metrics.py, a pure-Python recomputation of EVERY metric from those files
// alone, and requires agreement with summary.json. Nothing here reads data/.
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>

#include "bt/export.hpp"
#include "bt/strategies.hpp"
#include "bt/synthetic.hpp"
#include "doctest.h"
#include "test_util.hpp"

using namespace tu;
namespace fs = std::filesystem;

namespace {

CostConfig load_costs(const char* file) {
  std::ifstream in(std::string(BT_CONFIG_DIR) + "/" + file, std::ios::binary);
  REQUIRE(in.good());
  std::stringstream ss;
  ss << in.rdbuf();
  return CostConfig::from_json_string(ss.str());
}

std::vector<Bar> gbm(std::uint64_t seed, int bars, double price, double vol = 0.3, double drift = 0.08,
                     const char* start = "2022-01-03") {
  SynthParams p;
  p.start = parse_date(start).value();
  p.num_bars = bars;
  p.seed = seed;
  p.start_price = price;
  p.annual_vol = vol;
  p.annual_drift = drift;
  return generate_gbm(p);
}

// Drops each bar with probability p, and the first `skip_first` bars entirely (a leading gap).
std::vector<Bar> with_holes(const std::vector<Bar>& b, std::uint64_t seed, double p, std::size_t skip_first = 0) {
  std::mt19937_64 rng(seed);
  std::vector<Bar> out;
  for (std::size_t i = skip_first; i < b.size(); ++i) {
    if (static_cast<double>(rng() % 1000) / 1000.0 < p) continue;
    out.push_back(b[i]);
  }
  return out;
}

struct Written {
  EngineResult result;
  Analytics analytics;
};

Written write_scenario(const std::string& name, const AlignedData& d, Strategy& strat, const EngineConfig& ec,
                       const BenchmarkSeries* bench, const AnalyticsConfig& ac = {}) {
  Written w;
  w.result = Engine(d, ec).run(strat);
  w.analytics = analyze(w.result.equity_curve, w.result.trades, w.result.fills, ac, bench);
  export_run(std::string(BT_DIFF_DIR) + "/" + name, w.result, w.analytics, bench);
  return w;
}

EngineConfig ecfg(const CostConfig& c, double slip_bps = 0.0, bool shorts = false) {
  EngineConfig e;
  e.initial_cash = to_paise(1'000'000.0);
  e.costs = c;
  if (slip_bps > 0) {
    e.slippage.mode = SlippageConfig::Mode::FixedBps;
    e.slippage.value = slip_bps;
  }
  e.allow_short = shorts;
  return e;
}

BenchmarkSeries bench_of(const std::string& name, std::vector<Bar> bars) { return benchmark_from_bars(name, bars); }

}  // namespace

TEST_CASE("differential export: write the scenario folders that tools/ref_metrics.py re-computes") {
  fs::remove_all(BT_DIFF_DIR);  // never leave a stale scenario behind
  const CostConfig def = CostConfig::nse_delivery_defaults();
  const CostConfig f20 = load_costs("costs_nse_delivery_flat20.json");
  int scenarios = 0;

  // Universe: A full, B with holes, C listing late.
  const std::vector<Bar> a = gbm(11, 500, 1500), b = with_holes(gbm(12, 500, 800), 5, 0.1), c = gbm(13, 440, 3200);
  std::vector<Bar> c_late(c.begin() + 60, c.end());
  const AlignedData uni = align_calendar({{"A", a}, {"B", b}, {"C", c_late}});

  {  // 1. buy and hold; benchmark with a leading gap and holes (not part of the engine's data)
    auto s = make_strategy("buy_and_hold", nlohmann::json::parse(R"({"symbol":"A"})"));
    const BenchmarkSeries bm = bench_of("IDX1", with_holes(gbm(21, 500, 18000, 0.15), 6, 0.08, 40));
    const Written w = write_scenario("01_buy_hold_gappy_benchmark", uni, *s, ecfg(def), &bm);
    CHECK(w.analytics.benchmark->n_curve_dates_without_benchmark > 40);
    CHECK(w.result.open_positions.size() == 1);   // position still open at the end
    CHECK(w.analytics.trades.num_open_positions == 1);
    ++scenarios;
  }
  {  // 2. SMA on the symbol with holes, flat Rs 20 costs, 6 bps slippage, rf 4 %
    auto s = make_strategy("sma_crossover", nlohmann::json::parse(R"({"symbol":"B","fast":8,"slow":25})"));
    const BenchmarkSeries bm = bench_of("IDX2", a);
    AnalyticsConfig ac;
    ac.risk_free_annual = 0.04;
    const Written w = write_scenario("02_sma_holes_flat20_slippage", uni, *s, ecfg(f20, 6.0), &bm, ac);
    CHECK(w.analytics.trades.num_trades > 3);
    CHECK(w.analytics.trades.fees_pct_gross_pnl.has_value() == (w.analytics.trades.gross_pnl_paise.value() > 0));
    ++scenarios;
  }
  {  // 3. RSI, default costs, benchmark = another symbol's series, trading_days 250
    auto s = make_strategy("rsi_mean_reversion", nlohmann::json::parse(R"({"symbol":"A","period":7,"buy_below":35,"sell_above":60})"));
    const BenchmarkSeries bm = bench_of("IDX3", c_late);
    AnalyticsConfig ac;
    ac.trading_days = 250;
    const Written w = write_scenario("03_rsi_td250", uni, *s, ecfg(def), &bm, ac);
    CHECK(w.analytics.trades.num_trades > 0);
    ++scenarios;
  }
  {  // 4. Donchian with stops
    auto s = make_strategy("donchian_breakout", nlohmann::json::parse(R"({"symbol":"C","entry_n":15,"exit_n":7,"atr_period":10,"atr_mult":1.5})"));
    const BenchmarkSeries bm = bench_of("IDX4", b);
    const Written w = write_scenario("04_donchian_stops", uni, *s, ecfg(def, 3.0), &bm);
    CHECK(w.analytics.trades.num_trades > 0);
    ++scenarios;
  }
  {  // 5. momentum rotation over all three symbols
    auto s = make_strategy("momentum_rotation", nlohmann::json::parse(R"({"lookback":30,"top_k":2})"));
    const BenchmarkSeries bm = bench_of("IDX5", with_holes(gbm(25, 500, 100, 0.2), 8, 0.15));
    const Written w = write_scenario("05_momentum", uni, *s, ecfg(f20), &bm);
    CHECK(w.analytics.trades.num_fills > 5);
    ++scenarios;
  }
  {  // 6. zero trades (slow SMA never ready) and NO benchmark: most metrics null
    auto s = make_strategy("sma_crossover", nlohmann::json::parse(R"({"symbol":"A","fast":10,"slow":900})"));
    const Written w = write_scenario("06_zero_trades_no_benchmark", uni, *s, ecfg(def), nullptr);
    CHECK(w.analytics.trades.num_trades == 0);
    CHECK_FALSE(w.analytics.equity.sharpe.has_value());   // flat equity
    CHECK_FALSE(w.analytics.benchmark.has_value());
    ++scenarios;
  }
  {  // 7. all-winning: steadily rising price, three round trips, no losses -> profit factor null
    std::vector<Bar> up;
    for (int i = 0; i < 60; ++i) {
      const double px = 100.0 + i;
      up.push_back(mk(parse_date("2023-01-02").value() + i, px, px + 0.5, px - 0.5, px + 0.2));
    }
    const AlignedData d = one_symbol(up, "UP");
    Scripted s([](Context& cx) {
      const std::size_t t = cx.day_index();
      if (t == 2 || t == 22 || t == 42) cx.buy(0, 100);
      if (t == 12 || t == 32 || t == 52) cx.sell(0, cx.position(0));
    });
    const BenchmarkSeries bm = bench_of("IDX7", gbm(31, 60, 100, 0.1, 0.08, "2023-01-02"));
    const Written w = write_scenario("07_all_winning", d, s, ecfg(def), &bm);
    CHECK(w.analytics.trades.num_trades == 3);
    CHECK(w.analytics.trades.num_losses == 0);
    CHECK_FALSE(w.analytics.trades.profit_factor.has_value());
    ++scenarios;
  }
  {  // 8. all-losing: steadily falling price
    std::vector<Bar> down;
    for (int i = 0; i < 60; ++i) {
      const double px = 200.0 - i;
      down.push_back(mk(parse_date("2023-01-02").value() + i, px, px + 0.5, px - 0.5, px - 0.2));
    }
    const AlignedData d = one_symbol(down, "DOWN");
    Scripted s([](Context& cx) {
      const std::size_t t = cx.day_index();
      if (t == 2 || t == 22 || t == 42) cx.buy(0, 100);
      if (t == 12 || t == 32 || t == 52) cx.sell(0, cx.position(0));
    });
    const Written w = write_scenario("08_all_losing", d, s, ecfg(def), nullptr);
    CHECK(w.analytics.trades.num_wins == 0);
    CHECK(w.analytics.trades.num_losses == 3);
    CHECK(w.analytics.trades.profit_factor.value() == 0.0);
    CHECK_FALSE(w.analytics.trades.fees_pct_gross_pnl.has_value());   // gross P&L is negative
    ++scenarios;
  }
  {  // 9. shorting: short on a falling path, cover, short again and stay short at the end
    std::vector<Bar> v;
    for (int i = 0; i < 80; ++i) {
      const double px = 300.0 - 1.5 * i + ((i % 7) - 3);
      v.push_back(mk(parse_date("2023-03-01").value() + i, px, px + 1.0, px - 1.0, px));
    }
    const AlignedData d = one_symbol(v, "SH");
    Scripted s([](Context& cx) {
      const std::size_t t = cx.day_index();
      if (t == 3 || t == 43) cx.sell(0, 50);
      if (t == 30) cx.buy(0, 50);
    });
    const BenchmarkSeries bm = bench_of("IDX9", gbm(41, 80, 100, 0.2, 0.08, "2023-03-01"));
    const Written w = write_scenario("09_shorting_open_short", d, s, ecfg(f20, 0.0, true), &bm);
    CHECK(w.analytics.trades.num_trades == 1);
    CHECK(w.analytics.trades.num_open_positions == 1);
    CHECK(w.result.open_positions[0].qty == -50);
    ++scenarios;
  }
  {  // 10. 20 years of daily bars (summation order matters); SMA vs a long benchmark with holes
    const std::vector<Bar> big = gbm(51, 5000, 100, 0.25, 0.1);
    const AlignedData d = one_symbol(big, "BIG");
    auto s = make_strategy("sma_crossover", nlohmann::json::parse(R"({"fast":20,"slow":50})"));
    const BenchmarkSeries bm = bench_of("IDX10", with_holes(gbm(52, 5000, 10000, 0.18, 0.09), 9, 0.05, 25));
    const Written w = write_scenario("10_long_series_5000_bars", d, *s, ecfg(def, 2.0), &bm);
    CHECK(w.analytics.trades.num_trades > 30);
    CHECK(w.analytics.equity.n_records == 5000);
    ++scenarios;
  }
  {  // 11. benchmark that never moves (zero variance) and one with only 3 common dates
    auto s = make_strategy("buy_and_hold", nlohmann::json::parse(R"({"symbol":"A"})"));
    BenchmarkSeries flat;
    flat.name = "FLAT";
    for (const Bar& x : a) { flat.dates.push_back(x.date); flat.values.push_back(100.0); }
    const Written w = write_scenario("11_zero_variance_benchmark", uni, *s, ecfg(def), &flat);
    CHECK_FALSE(w.analytics.benchmark->beta.has_value());
    ++scenarios;
    BenchmarkSeries few;
    few.name = "FEW";
    few.dates = {a[10].date, a[200].date, a[400].date};
    few.values = {100, 130, 120};
    auto s2 = make_strategy("buy_and_hold", nlohmann::json::parse(R"({"symbol":"A"})"));
    const Written w2 = write_scenario("12_three_common_dates", uni, *s2, ecfg(def), &few);
    CHECK(w2.analytics.benchmark->n_pairs == 2);
    CHECK_FALSE(w2.analytics.benchmark->beta.has_value());
    ++scenarios;
  }
  {  // 13. a benchmark that shares NO dates with the strategy curve
    auto s = make_strategy("buy_and_hold", nlohmann::json::parse(R"({"symbol":"A"})"));
    BenchmarkSeries far;
    far.name = "FAR";
    far.dates = {parse_date("2010-01-04").value(), parse_date("2010-01-05").value()};
    far.values = {1, 2};
    const Written w = write_scenario("13_no_common_dates", uni, *s, ecfg(def), &far);
    CHECK(w.analytics.benchmark->n_common_dates == 0);
    ++scenarios;
  }
  CHECK(scenarios == 13);
  // every folder has the mandatory files
  int folders = 0;
  for (const auto& e : fs::directory_iterator(BT_DIFF_DIR)) {
    ++folders;
    for (const char* f : {"equity.csv", "trades.csv", "fills.csv", "summary.json"}) CHECK(fs::exists(e.path() / f));
  }
  CHECK(folders == 13);
}
