// Analytics tests. Curves/trades/fills are built by hand so every expected number can be derived on paper (the arithmetic
// is in the comments; the non-trivial literals were computed independently in plain Python, not with the code under test).
#include <algorithm>
#include <cctype>
#include <cmath>
#include <set>
#include <sstream>

#include "bt/analytics.hpp"
#include "bt/export.hpp"
#include "bt/strategies.hpp"
#include "bt/synthetic.hpp"
#include "doctest.h"
#include "test_util.hpp"

using namespace tu;
using nlohmann::json;

namespace {

const Date kD0 = parse_date("2024-01-01").value();

struct Pt { int off; Paise equity; Paise pos; };

std::vector<DailyRecord> curve_of(const std::vector<Pt>& pts) {
  std::vector<DailyRecord> v;
  for (const Pt& p : pts) v.push_back({kD0 + p.off, p.equity - p.pos, p.pos, p.equity});
  return v;
}

Analytics run(const std::vector<DailyRecord>& c, double rf = 0.0, const std::vector<Trade>& t = {},
              const std::vector<Fill>& f = {}, const BenchmarkSeries* b = nullptr) {
  AnalyticsConfig cfg;
  cfg.risk_free_annual = rf;
  return analyze(c, t, f, cfg, b);
}

#define CHECK_NEAR(opt, expected)                                           \
  do {                                                                      \
    REQUIRE((opt).has_value());                                             \
    CHECK(*(opt) == doctest::Approx(expected).epsilon(1e-9));               \
  } while (0)

// Test curve A (paise).  Day offsets 0,1,2,3,365.   equity: 10000 11000 9900 10890 11979
//   returns: 11000/10000-1 = +10 %,  9900/11000-1 = -10 %,  10890/9900-1 = +10 %,  11979/10890-1 = +10 %
//   position values: 0, 5000, 4000, 0, 0
std::vector<DailyRecord> curve_a() {
  return curve_of({{0, 10000, 0}, {1, 11000, 5000}, {2, 9900, 4000}, {3, 10890, 0}, {365, 11979, 0}});
}

Trade mk_trade(Paise net, int entry_off, int exit_off, std::size_t entry_day, std::size_t exit_day) {
  Trade t;
  t.symbol = 0;
  t.entry_date = kD0 + entry_off;
  t.exit_date = kD0 + exit_off;
  t.entry_day = entry_day;
  t.exit_day = exit_day;
  t.net_pnl = net;
  t.gross_pnl = net;
  t.qty = 1;
  return t;
}

Fill mk_fill(std::size_t sym, Side side, Paise notional, Paise brokerage, std::int64_t pos_after) {
  Fill f;
  f.symbol = sym;
  f.side = side;
  f.notional = notional;
  f.fees.brokerage = brokerage;
  f.position_after = pos_after;
  return f;
}

}  // namespace

// ===================================================================== equity metrics
TEST_CASE("equity metrics: hand-computed on curve A with risk-free 0") {
  const Analytics a = run(curve_a(), 0.0);
  const EquityMetrics& e = a.equity;
  CHECK(e.n_records == 5);
  CHECK(e.start_equity.value() == 10000);
  CHECK(e.end_equity.value() == 11979);
  CHECK(*e.start_date == kD0);
  CHECK(*e.end_date == kD0 + 365);
  CHECK_NEAR(e.total_return, 0.1979);                  // 11979/10000 - 1
  CHECK_NEAR(e.years, 365.0 / 365.25);
  CHECK_NEAR(e.cagr, 0.19804816315622786);             // 1.1979^(365.25/365) - 1
  CHECK_NEAR(e.mean_daily_return, 0.05);               // (0.1 - 0.1 + 0.1 + 0.1) / 4
  CHECK_NEAR(e.daily_return_sd, 0.1);                  // deviations .05 -.15 .05 .05 -> sum sq .03 -> /3 = .01 -> sqrt .1
  CHECK_NEAR(e.annualised_volatility, 0.1 * std::sqrt(252.0));  // = 1.587450786638755
  CHECK_NEAR(e.sharpe, 7.937253933193772);             // 0.05 / 0.1 * sqrt(252)
  CHECK_NEAR(e.sortino, 15.874507866387544);           // downside dev = sqrt(0.1^2 / 4) = 0.05 -> 0.05/0.05*sqrt(252)
  CHECK_NEAR(e.max_drawdown, -0.1);                    // 9900/11000 - 1
  CHECK(*e.max_drawdown_peak_date == kD0 + 1);
  CHECK(*e.max_drawdown_trough_date == kD0 + 2);
  CHECK(*e.max_drawdown_recovery_date == kD0 + 365);   // 10890 < 11000, then 11979 >= 11000
  CHECK(e.max_drawdown_recovered.value() == true);
  CHECK(*e.max_drawdown_duration_days == 364);         // peak day 1 -> recovery day 365
  CHECK(*e.longest_underwater_days == 364);
  CHECK_NEAR(e.calmar, 1.9804816315622786);            // cagr / 0.1
  CHECK_NEAR(e.best_day, 0.1);
  CHECK(*e.best_day_date == kD0 + 1);                  // three +10 % days tie: the FIRST wins
  CHECK_NEAR(e.worst_day, -0.1);
  CHECK(*e.worst_day_date == kD0 + 2);
  CHECK_NEAR(e.time_in_market, 0.4);                   // 2 of 5 records hold a position
  CHECK_NEAR(e.avg_exposure, 17.0 / 99.0);             // (5000/11000 + 4000/9900)/5 = (45/99 + 40/99)/5
}

TEST_CASE("equity metrics: a 6.5 % risk-free rate changes Sharpe and Sortino as computed by hand") {
  // rf_d = 0.065/252 = 2.5793650793650794e-4;  mean excess = 0.05 - rf_d = 0.04974206349206349
  const Analytics a = run(curve_a(), 0.065);
  CHECK_NEAR(a.equity.sharpe, 7.896307781951115);      // 0.049742.../0.1*sqrt(252)
  CHECK_NEAR(a.equity.sortino, 15.751985442720617);    // negative-day excess is -0.1 - rf_d, so DD is slightly larger
  CHECK_NEAR(a.equity.mean_daily_return, 0.05);        // mean(r) itself is unaffected
  CHECK_NEAR(a.equity.annualised_volatility, 1.587450786638755);
}

TEST_CASE("equity metrics: CAGR over exactly four years (1461 days = 4 x 365.25)") {
  const Date a = parse_date("2020-01-01").value(), b = parse_date("2024-01-01").value();
  REQUIRE(b - a == 1461);
  const Analytics r = analyze({{a, 0, 0, 10000}, {b, 0, 0, 20000}}, {}, {});
  CHECK_NEAR(r.equity.years, 4.0);
  CHECK_NEAR(r.equity.cagr, 0.18920711500272103);      // 2^(1/4) - 1
  CHECK_NEAR(r.equity.total_return, 1.0);
  // One return only: spread statistics are undefined, not silently zero or NaN.
  CHECK_FALSE(r.equity.daily_return_sd.has_value());
  CHECK_FALSE(r.equity.annualised_volatility.has_value());
  CHECK_FALSE(r.equity.sharpe.has_value());
  CHECK(r.equity.max_drawdown.value() == 0.0);
}

TEST_CASE("equity metrics: drawdown not recovered, recovery on equality, tie goes to the first trough, two spells") {
  {  // 10000, 12000, 9000, 9600: peak idx1, trough idx2, never back to 12000
    const auto e = run(curve_of({{0, 10000, 0}, {10, 12000, 0}, {20, 9000, 0}, {30, 9600, 0}})).equity;
    CHECK_NEAR(e.max_drawdown, -0.25);
    CHECK(*e.max_drawdown_peak_date == kD0 + 10);
    CHECK(*e.max_drawdown_trough_date == kD0 + 20);
    CHECK_FALSE(e.max_drawdown_recovery_date.has_value());
    CHECK(e.max_drawdown_recovered.value() == false);
    CHECK(*e.max_drawdown_duration_days == 20);          // peak (day 10) -> last date (day 30)
    CHECK(*e.longest_underwater_days == 20);
  }
  {  // recovery exactly AT the old peak counts as recovered
    const auto e = run(curve_of({{0, 10000, 0}, {1, 12000, 0}, {2, 9000, 0}, {5, 12000, 0}})).equity;
    CHECK(e.max_drawdown_recovered.value() == true);
    CHECK(*e.max_drawdown_recovery_date == kD0 + 5);
    CHECK(*e.max_drawdown_duration_days == 4);
  }
  {  // plateau AT the peak: the peak DATE is the first date the level was reached (100@0, 100@1, 90@2, 100@3)
    const auto e = run(curve_of({{0, 100, 0}, {1, 100, 0}, {2, 90, 0}, {3, 100, 0}})).equity;
    CHECK_NEAR(e.max_drawdown, -0.1);
    CHECK(*e.max_drawdown_peak_date == kD0);              // not day 1
    CHECK(*e.max_drawdown_duration_days == 3);            // peak day 0 -> recovery day 3
    CHECK(*e.longest_underwater_days == 3);
  }
  {  // two equal -10 % drawdowns: the earlier trough is reported. Peak 100 -> 90 (day 1..2), later peak 200 -> 180
    const auto e = run(curve_of({{0, 100, 0}, {1, 90, 0}, {2, 100, 0}, {3, 200, 0}, {4, 180, 0}, {9, 200, 0}})).equity;
    CHECK_NEAR(e.max_drawdown, -0.1);
    CHECK(*e.max_drawdown_trough_date == kD0 + 1);       // first occurrence
    CHECK(*e.max_drawdown_peak_date == kD0);
    // spells: peak day 0 -> recovery day 2 (2 days); peak day 3 -> recovery day 9 (6 days). Longest = 6.
    CHECK(*e.longest_underwater_days == 6);
    CHECK(*e.max_drawdown_duration_days == 2);
  }
}

TEST_CASE("equity metrics: underwater spells are measured against the RUNNING maximum, not every older peak") {
  // 100@day0, 110@day100 (new high), 95@101, 108@102 (back above the OLD peak 100 but still below 110), 109@120, 111@150.
  // The only spell: peak 110 (day 100) until the first record >= 110 (day 150) = 50 days.
  // (A buggy variant that also started a spell at the old peak 100 would report 102.)
  const auto e = run(curve_of({{0, 100, 0}, {100, 110, 0}, {101, 95, 0}, {102, 108, 0}, {120, 109, 0}, {150, 111, 0}})).equity;
  CHECK(*e.longest_underwater_days == 50);
  CHECK(*e.max_drawdown_duration_days == 50);
  CHECK(*e.max_drawdown_peak_date == kD0 + 100);
  CHECK(*e.max_drawdown_recovery_date == kD0 + 150);
}

TEST_CASE("equity metrics: exposure counts SHORT positions by absolute value") {
  // positions 0, -5000, -4000 on equity 10000, 10000, 8000: |pos|/equity = 0, 0.5, 0.5
  const auto e = run(curve_of({{0, 10000, 0}, {1, 10000, -5000}, {2, 8000, -4000}})).equity;
  CHECK_NEAR(e.time_in_market, 2.0 / 3.0);
  CHECK_NEAR(e.avg_exposure, 1.0 / 3.0);
}

TEST_CASE("equity metrics: flat curve, tiny curves and a non-positive curve give DEFINED outputs") {
  {  // flat: zero volatility -> Sharpe and Sortino undefined (null), no drawdown, no NaN
    const auto e = run(curve_of({{0, 10000, 0}, {1, 10000, 0}, {2, 10000, 0}, {3, 10000, 0}})).equity;
    CHECK(e.total_return.value() == 0.0);
    CHECK(e.mean_daily_return.value() == 0.0);
    CHECK(e.daily_return_sd.value() == 0.0);
    CHECK_FALSE(e.sharpe.has_value());
    CHECK_FALSE(e.sortino.has_value());               // downside deviation is 0 (rf = 0)
    CHECK(e.max_drawdown.value() == 0.0);
    CHECK_FALSE(e.max_drawdown_peak_date.has_value());
    CHECK_FALSE(e.max_drawdown_recovered.has_value());
    CHECK(*e.max_drawdown_duration_days == 0);
    CHECK(*e.longest_underwater_days == 0);
    CHECK_FALSE(e.calmar.has_value());                // no drawdown to divide by
    CHECK(e.cagr.value() == 0.0);
  }
  {  // empty curve
    const auto e = run({}).equity;
    CHECK(e.n_records == 0);
    CHECK_FALSE(e.start_date.has_value());
    CHECK_FALSE(e.total_return.has_value());
    CHECK_FALSE(e.time_in_market.has_value());
    CHECK_FALSE(e.max_drawdown.has_value());
  }
  {  // a single record: nothing to compare against
    const auto e = run(curve_of({{0, 10000, 0}})).equity;
    CHECK(e.n_records == 1);
    CHECK(e.total_return.value() == 0.0);
    CHECK_FALSE(e.years.has_value());
    CHECK_FALSE(e.cagr.has_value());
    CHECK_FALSE(e.mean_daily_return.has_value());
    CHECK_FALSE(e.sharpe.has_value());
    CHECK(e.max_drawdown.value() == 0.0);
  }
  {  // equity reaches zero: ratios are meaningless, so every return-based metric is null (not NaN/inf)
    const auto e = run(curve_of({{0, 10000, 0}, {1, 5000, 0}, {2, 0, 0}, {3, 0, 0}})).equity;
    CHECK(e.total_return.value() == -1.0);
    CHECK_FALSE(e.cagr.has_value());
    CHECK_FALSE(e.mean_daily_return.has_value());
    CHECK_FALSE(e.sharpe.has_value());
    CHECK_FALSE(e.max_drawdown.has_value());
    CHECK_FALSE(e.best_day.has_value());
  }
  CHECK_THROWS_AS(analyze(curve_of({{1, 100, 0}, {1, 100, 0}}), {}, {}), std::invalid_argument);  // duplicate dates
  AnalyticsConfig bad;
  bad.trading_days = 0;
  CHECK_THROWS_AS(analyze({}, {}, {}, bad), std::invalid_argument);
}

// ===================================================================== trade metrics
TEST_CASE("trade metrics: hand-computed (net P&L; zero-P&L trades are neither wins nor losses)") {
  // net P&L (paise): +300, -100, +200, -400, 0     holding days 5,10,3,20,1      holding bars 3,7,2,14,1
  const std::vector<Trade> t = {mk_trade(300, 0, 5, 10, 13), mk_trade(-100, 0, 10, 0, 7), mk_trade(200, 0, 3, 5, 7),
                                mk_trade(-400, 0, 20, 0, 14), mk_trade(0, 0, 1, 3, 4)};
  const TradeMetrics m = run(curve_a(), 0.0, t).trades;
  CHECK(m.num_trades == 5);
  CHECK(m.num_wins == 2);
  CHECK(m.num_losses == 2);
  CHECK(m.num_breakeven == 1);
  CHECK_NEAR(m.win_rate, 0.4);              // 2 / 5 (the break-even trade stays in the denominator)
  CHECK_NEAR(m.avg_win_paise, 250.0);       // (300 + 200) / 2
  CHECK_NEAR(m.avg_loss_paise, -250.0);     // (-100 - 400) / 2
  CHECK_NEAR(m.profit_factor, 1.0);         // 500 / |-500|
  CHECK(m.best_trade_paise.value() == 300);
  CHECK(m.worst_trade_paise.value() == -400);
  CHECK_NEAR(m.avg_holding_days, 7.8);      // (5+10+3+20+1)/5
  CHECK_NEAR(m.avg_holding_bars, 5.4);      // (3+7+2+14+1)/5
}

TEST_CASE("trade metrics: all-winning (profit factor null), all-losing (profit factor 0), no trades") {
  {
    const TradeMetrics m = run(curve_a(), 0.0, {mk_trade(100, 0, 1, 0, 1), mk_trade(50, 0, 1, 0, 1)}).trades;
    CHECK(m.num_wins == 2);
    CHECK_FALSE(m.profit_factor.has_value());     // infinite -> null
    CHECK_FALSE(m.avg_loss_paise.has_value());
    CHECK_NEAR(m.win_rate, 1.0);
    CHECK_NEAR(m.avg_win_paise, 75.0);
  }
  {
    const TradeMetrics m = run(curve_a(), 0.0, {mk_trade(-100, 0, 1, 0, 1), mk_trade(-50, 0, 1, 0, 1)}).trades;
    CHECK(m.num_losses == 2);
    CHECK_NEAR(m.profit_factor, 0.0);             // 0 / 150
    CHECK_FALSE(m.avg_win_paise.has_value());
    CHECK_NEAR(m.win_rate, 0.0);
  }
  {
    const TradeMetrics m = run(curve_a()).trades;
    CHECK(m.num_trades == 0);
    CHECK_FALSE(m.win_rate.has_value());
    CHECK_FALSE(m.profit_factor.has_value());
    CHECK_FALSE(m.avg_holding_days.has_value());
    CHECK_FALSE(m.best_trade_paise.has_value());
    CHECK(m.total_fees_paise == 0);
    CHECK_NEAR(m.fees_pct_initial_capital, 0.0);
  }
}

TEST_CASE("trade metrics: turnover, fees, open positions from fills (hand-computed on curve A)") {
  // fills: buy 10,000 (fee 100, position 100), sell 10,800 (fee 50, position 0)
  //   traded value 20,800.  mean equity (10000+11000+9900+10890+11979)/5 = 10,753.8
  //   turnover = 20,800 / (2 * 10,753.8) = 0.96710000186;  annual = / (365/365.25) = 0.96776239912
  //   net P&L = 11979 - 10000 = 1979;  fees 150;  gross = 2129;  fees/gross = 0.070455612964;  fees/start = 0.015
  const std::vector<Fill> f = {mk_fill(0, Side::Buy, 10000, 100, 100), mk_fill(0, Side::Sell, 10800, 50, 0)};
  const TradeMetrics m = run(curve_a(), 0.0, {}, f).trades;
  CHECK(m.num_fills == 2);
  CHECK(m.traded_value_paise == 20800);
  CHECK(m.total_fees_paise == 150);
  CHECK_NEAR(m.average_equity_paise, 10753.8);
  CHECK_NEAR(m.turnover, 0.9671000018598077);
  CHECK_NEAR(m.turnover_annual, 0.9677623991213555);
  CHECK(m.net_pnl_paise.value() == 1979);
  CHECK(m.gross_pnl_paise.value() == 2129);
  CHECK_NEAR(m.fees_pct_gross_pnl, 0.07045561296383279);
  CHECK_NEAR(m.fees_pct_initial_capital, 0.015);
  CHECK(m.num_open_positions == 0);

  // a second symbol whose last fill leaves a position -> one open position
  std::vector<Fill> g = f;
  g.push_back(mk_fill(1, Side::Buy, 500, 0, 7));
  CHECK(run(curve_a(), 0.0, {}, g).trades.num_open_positions == 1);
}

TEST_CASE("trade metrics: fees as % of gross P&L is null when gross P&L <= 0, but % of capital is defined") {
  const auto losing = curve_of({{0, 10000, 0}, {1, 9000, 0}, {2, 8000, 0}});
  const TradeMetrics m = run(losing, 0.0, {}, {mk_fill(0, Side::Buy, 5000, 100, 1)}).trades;
  CHECK(m.net_pnl_paise.value() == -2000);
  CHECK(m.gross_pnl_paise.value() == -1900);          // still a loss before fees
  CHECK_FALSE(m.fees_pct_gross_pnl.has_value());
  CHECK_NEAR(m.fees_pct_initial_capital, 0.01);        // 100 / 10000
}

// ===================================================================== benchmark
namespace {
// Strategy equity on days 0..5 (paise); the benchmark has NO bar on day 2.
std::vector<DailyRecord> bench_curve() {
  return curve_of({{0, 10'000'000, 0}, {1, 10'200'000, 0}, {2, 10'300'000, 0}, {3, 10'608'000, 0}, {4, 10'395'840, 0},
                   {5, 10'915'632, 0}});
}
BenchmarkSeries bench_series() {
  BenchmarkSeries b;
  b.name = "TESTIDX";
  b.dates = {kD0 + 0, kD0 + 1, kD0 + 3, kD0 + 4, kD0 + 5};
  b.values = {100.0, 101.0, 103.02, 101.9898, 104.539545};
  return b;
}
}  // namespace

TEST_CASE("benchmark: returns are paired between consecutive COMMON dates over the same interval (hand-computed)") {
  // Common dates: 0,1,3,4,5.  Strategy returns over those intervals: +2 %, 10,608,000/10,200,000 = +4 % (spans the gap day 2),
  // -2 %, +5 %.  Benchmark returns: +1 %, +2 %, -1 %, +2.5 %.  Strategy = exactly 2 x benchmark => beta 2, correlation 1,
  // alpha 0 (rf = 0).  With rf = 6.5 %: alpha = (ms - rf_d - 2(mb - rf_d)) * 252 = rf_d * 252 = 0.065.
  const BenchmarkSeries b = bench_series();
  const BenchmarkMetrics m = *run(bench_curve(), 0.0, {}, {}, &b).benchmark;
  CHECK(m.name == "TESTIDX");
  CHECK(m.n_common_dates == 5);
  CHECK(m.n_pairs == 4);
  CHECK(m.n_curve_dates_without_benchmark == 1);     // day 2
  CHECK(m.n_benchmark_dates_not_in_curve == 0);
  CHECK(*m.first_common_date == kD0);
  CHECK(*m.last_common_date == kD0 + 5);
  CHECK_NEAR(m.beta, 2.0);
  CHECK_NEAR(m.correlation, 1.0);
  CHECK(std::fabs(*m.alpha_annual) < 1e-9);
  CHECK_NEAR(m.strategy_return, 0.0915632);           // 10,915,632 / 10,000,000 - 1
  CHECK_NEAR(m.benchmark_return, 0.04539545);         // 104.539545 / 100 - 1
  CHECK_NEAR(m.excess_return, 0.04616775);
  CHECK_NEAR(run(bench_curve(), 0.065, {}, {}, &b).benchmark->alpha_annual, 0.065);
}

TEST_CASE("benchmark: forward-filling the benchmark would give a clearly different (wrong) beta") {
  // Naive alternative: fill the benchmark's missing day 2 with day 1's value and use 1-day returns for both series.
  // That injects a 0 % benchmark return paired with the strategy's +0.98 % on day 2, and then a 2-day benchmark move
  // (+2 %) paired with a 1-day strategy move. Its beta (computed independently in Python) is 1.7569..., not 2.
  const auto c = bench_curve();
  const BenchmarkSeries b = bench_series();
  std::vector<double> rs, rb;
  double prev_b = b.values[0];
  std::size_t bi = 0;
  for (std::size_t i = 1; i < c.size(); ++i) {
    double cur_b = prev_b;
    while (bi < b.dates.size() && b.dates[bi] <= c[i].date) cur_b = b.values[bi++];
    rs.push_back(static_cast<double>(c[i].equity) / static_cast<double>(c[i - 1].equity) - 1.0);
    rb.push_back(cur_b / prev_b - 1.0);
    prev_b = cur_b;
  }
  double ms = 0, mb = 0;
  for (std::size_t i = 0; i < rs.size(); ++i) { ms += rs[i]; mb += rb[i]; }
  ms /= rs.size();
  mb /= rb.size();
  double cov = 0, var = 0;
  for (std::size_t i = 0; i < rs.size(); ++i) { cov += (rs[i] - ms) * (rb[i] - mb); var += (rb[i] - mb) * (rb[i] - mb); }
  const double naive_beta = cov / var;
  CHECK(naive_beta == doctest::Approx(1.756947249655257).epsilon(1e-9));
  CHECK(std::fabs(naive_beta - 2.0) > 0.2);
}

TEST_CASE("benchmark: undefined cases give nulls with the counts still reported") {
  {  // zero-variance benchmark: beta / alpha / correlation undefined, returns still reported
    BenchmarkSeries b = bench_series();
    b.values = {100, 100, 100, 100, 100};
    const BenchmarkMetrics m = *run(bench_curve(), 0.0, {}, {}, &b).benchmark;
    CHECK_FALSE(m.beta.has_value());
    CHECK_FALSE(m.alpha_annual.has_value());
    CHECK_FALSE(m.correlation.has_value());
    CHECK_NEAR(m.benchmark_return, 0.0);
    CHECK(m.n_pairs == 4);
  }
  {  // only 3 common dates (2 pairs): too few for a covariance
    BenchmarkSeries b;
    b.name = "X";
    b.dates = {kD0 + 0, kD0 + 3, kD0 + 5};
    b.values = {100, 102, 101};
    const BenchmarkMetrics m = *run(bench_curve(), 0.0, {}, {}, &b).benchmark;
    CHECK(m.n_common_dates == 3);
    CHECK(m.n_pairs == 2);
    CHECK_FALSE(m.beta.has_value());
    CHECK_NEAR(m.strategy_return, 0.0915632);
    CHECK_NEAR(m.benchmark_return, 0.01);
  }
  {  // no overlap at all
    BenchmarkSeries b;
    b.name = "X";
    b.dates = {kD0 + 100, kD0 + 101};
    b.values = {1, 2};
    const BenchmarkMetrics m = *run(bench_curve(), 0.0, {}, {}, &b).benchmark;
    CHECK(m.n_common_dates == 0);
    CHECK(m.n_pairs == 0);
    CHECK(m.n_curve_dates_without_benchmark == 6);
    CHECK(m.n_benchmark_dates_not_in_curve == 2);
    CHECK_FALSE(m.first_common_date.has_value());
    CHECK_FALSE(m.strategy_return.has_value());
  }
  CHECK_FALSE(run(bench_curve()).benchmark.has_value());  // none supplied
  BenchmarkSeries bad = bench_series();
  bad.dates[2] = bad.dates[1];  // not strictly increasing
  CHECK_THROWS_AS(run(bench_curve(), 0.0, {}, {}, &bad), std::invalid_argument);
  bad = bench_series();
  bad.values[1] = 0.0;
  CHECK_THROWS_AS(run(bench_curve(), 0.0, {}, {}, &bad), std::invalid_argument);
}

TEST_CASE("benchmark: supplying one never changes a strategy's own results (it is not in the engine's calendar)") {
  SynthParams p;
  p.start = parse_date("2022-01-03").value();
  p.num_bars = 300;
  p.seed = 3;
  const AlignedData d = one_symbol(generate_gbm(p), "S");
  auto strat = make_strategy("sma_crossover", json::parse(R"({"fast": 5, "slow": 20})"));
  const EngineResult r = Engine(d, engine_cfg(100'000'000, CostConfig::nse_delivery_defaults())).run(*strat);
  // A benchmark with extra dates (a weekend) and missing dates.
  BenchmarkSeries b;
  b.name = "B";
  for (std::size_t i = 0; i < d.num_days(); i += 2) { b.dates.push_back(d.date(i)); b.values.push_back(100.0 + static_cast<double>(i)); }
  b.dates.push_back(d.date(d.num_days() - 1) + 1);
  b.values.push_back(500.0);
  const Analytics without = analyze(r.equity_curve, r.trades, r.fills);
  const Analytics with = analyze(r.equity_curve, r.trades, r.fills, {}, &b);
  CHECK(to_json(with)["equity"] == to_json(without)["equity"]);
  CHECK(to_json(with)["trades"] == to_json(without)["trades"]);
  CHECK(with.benchmark->n_benchmark_dates_not_in_curve == 1);
}

// ===================================================================== JSON & export format
TEST_CASE("json: undefined metrics are null, no NaN/inf text, and the key set never depends on the data") {
  const json empty = to_json(run({}));
  const json flat = to_json(run(curve_of({{0, 10000, 0}, {1, 10000, 0}, {2, 10000, 0}})));
  const json full = to_json(run(curve_a(), 0.065, {mk_trade(5, 0, 1, 0, 1)}, {mk_fill(0, Side::Buy, 10, 1, 1)}));
  for (const char* section : {"equity", "trades", "config"}) {
    std::set<std::string> k1, k2, k3;
    for (auto it = empty[section].begin(); it != empty[section].end(); ++it) k1.insert(it.key());
    for (auto it = flat[section].begin(); it != flat[section].end(); ++it) k2.insert(it.key());
    for (auto it = full[section].begin(); it != full[section].end(); ++it) k3.insert(it.key());
    CHECK(k1 == k2);
    CHECK(k2 == k3);
  }
  CHECK(flat["equity"]["sharpe"].is_null());
  CHECK(flat["equity"]["max_drawdown_peak_date"].is_null());
  CHECK(empty["equity"]["start_date"].is_null());
  CHECK(full["equity"]["start_date"] == "2024-01-01");
  CHECK(full["benchmark"].is_null());
  for (const json* j : {&empty, &flat, &full}) {
    std::string s = j->dump();
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char ch) { return std::tolower(ch); });
    CHECK(s.find("nan") == std::string::npos);
    CHECK(s.find("inf") == std::string::npos);
  }
  // integers stay integers and doubles stay doubles (the Python checker relies on that distinction)
  CHECK(full["equity"]["n_records"].is_number_integer());
  CHECK(full["equity"]["start_equity_paise"].is_number_integer());
  CHECK(full["equity"]["sharpe"].is_number_float());
  CHECK(full["config"]["risk_free_annual"] == 0.065);
  CHECK(full["config"]["trading_days_per_year"] == 252);
}

TEST_CASE("export: CSV layouts") {
  std::ostringstream eq;
  write_equity_csv(eq, curve_of({{0, 10000, 0}, {1, 11000, 5000}}));
  CHECK(eq.str() == "date,cash_paise,positions_value_paise,equity_paise\n2024-01-01,10000,0,10000\n2024-01-02,6000,5000,11000\n");

  Trade t = mk_trade(300, 0, 5, 10, 13);
  t.direction = Side::Sell;
  t.qty = 7;
  t.entry_value = 1000;
  t.exit_value = 700;
  t.gross_pnl = 300;
  t.fees.brokerage = 20;
  t.net_pnl = 280;
  t.num_fills = 2;
  std::ostringstream tr;
  write_trades_csv(tr, {t}, {"ABC"});
  CHECK(tr.str().substr(tr.str().find('\n') + 1) == "ABC,short,2024-01-01,2024-01-06,10,13,7,1000,700,300,20,280,2\n");

  std::ostringstream fl;
  Fill f = mk_fill(0, Side::Buy, 12345, 10, 5);
  f.order_id = 4;
  f.date = kD0;
  f.day_index = 9;
  f.qty = 5;
  f.price = 2469;
  f.fees.stt = 12;
  f.cash_after = 777;
  write_fills_csv(fl, {f}, {"ABC"});
  const std::string fs = fl.str();
  CHECK(fs.substr(fs.find('\n') + 1) == "4,2024-01-01,9,ABC,buy,5,2469,12345,10,12,0,0,0,0,22,777,5\n");
  // header and row have the same number of columns
  const std::string header = fs.substr(0, fs.find('\n'));
  const std::string row = fs.substr(fs.find('\n') + 1);
  CHECK(std::count(header.begin(), header.end(), ',') == std::count(row.begin(), row.end(), ','));

  std::ostringstream bn;
  BenchmarkSeries b;
  b.dates = {kD0};
  b.values = {103.02};
  write_benchmark_csv(bn, b);
  CHECK(bn.str() == "date,value\n2024-01-01,103.02\n");
}
