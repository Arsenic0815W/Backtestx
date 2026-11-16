#pragma once
// Performance analytics. THE INPUT CONTRACT: analyze() reads only the plain rows that are also exported to CSV
// (equity records, closed round-trip trades, fills) plus a separate benchmark series. tools/ref_metrics.py recomputes every
// metric from those exported files alone, so no metric may use anything that is not exported.
//
// Every metric that is undefined for the given data is std::nullopt (serialised as JSON null) - never a silent NaN.
// See LEARNING.md (Phase 4) for the formula of every metric; the key conventions are repeated here.
//
//  Return series   r_i = E_i / E_{i-1} - 1 over consecutive equity-curve records. The curve is on the UNION calendar of all
//                  loaded symbols, so a day on which nothing the strategy holds traded is a zero-return day (this slightly
//                  lowers volatility). Annualisation uses trading_days (252), a convention that need not match the calendar.
//  Starting equity = the FIRST curve record (not initial cash), so a window-trimmed curve works unchanged.
//  CAGR            (E_last/E_first)^(1/years) - 1,  years = (last date - first date) / 365.25 CALENDAR days.
//  Volatility      sample standard deviation (n-1) of r, times sqrt(trading_days).
//  Sharpe          (mean(r) - rf_d) / sd(r) * sqrt(trading_days),  rf_d = risk_free_annual / trading_days.
//  Sortino         (mean(r) - rf_d) / DD * sqrt(trading_days),  DD = sqrt( sum(min(r_i - rf_d, 0)^2) / n ): target = the
//                  daily risk-free rate, denominator = ALL n returns (not only the negative ones).
//  Drawdown        dd_i = E_i / (running max of E up to i) - 1. Max drawdown = the minimum (first occurrence on ties).
//                  Duration = calendar days from the peak date to the recovery date (first later date with E >= peak), or to
//                  the last date if never recovered. Longest underwater = the longest such peak-to-recovery spell.
//  Trades          CLOSED round trips only (flat -> flat), on NET P&L (after fees). Zero-P&L trades are neither wins nor losses.
//  Turnover        sum of fill notionals / (2 * mean equity)  (one-way); annual = / years.
//  Benchmark       both series use returns between consecutive COMMON dates (dates present in both), over the same interval;
//                  nothing is ever forward-filled. alpha = regression intercept on excess returns, times trading_days.
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "bt/engine.hpp"
#include "json.hpp"

namespace bt {

struct AnalyticsConfig {
  // ASSUMPTION, not a verified figure: roughly the Indian risk-free rate. Sharpe/Sortino/alpha move with it
  // (Sharpe changes by about rf / volatility, e.g. 0.065 / 0.20 = 0.33 at 20 % volatility).
  double risk_free_annual = 0.065;
  int trading_days = 252;
};

// A benchmark price/level series kept OUTSIDE the engine's AlignedData, so supplying one can never add calendar days or
// change a strategy's own equity curve. Dates strictly increasing, values > 0.
struct BenchmarkSeries {
  std::string name;
  std::vector<Date> dates;
  std::vector<double> values;
};

BenchmarkSeries benchmark_from_bars(const std::string& name, const std::vector<Bar>& bars);  // uses Close

struct EquityMetrics {
  std::int64_t n_records = 0;
  std::optional<Date> start_date, end_date;
  std::optional<Paise> start_equity, end_equity;
  std::optional<double> total_return;
  std::optional<double> years;
  std::optional<double> cagr;
  std::optional<double> mean_daily_return;
  std::optional<double> daily_return_sd;
  std::optional<double> annualised_volatility;
  std::optional<double> sharpe;
  std::optional<double> sortino;
  std::optional<double> max_drawdown;  // <= 0
  std::optional<Date> max_drawdown_peak_date, max_drawdown_trough_date, max_drawdown_recovery_date;
  std::optional<bool> max_drawdown_recovered;
  std::optional<std::int64_t> max_drawdown_duration_days;
  std::optional<std::int64_t> longest_underwater_days;
  std::optional<double> calmar;
  std::optional<double> best_day, worst_day;
  std::optional<Date> best_day_date, worst_day_date;
  std::optional<double> time_in_market;  // share of records with a non-zero position value
  std::optional<double> avg_exposure;    // mean |positions value| / equity
};

struct TradeMetrics {
  std::int64_t num_trades = 0, num_wins = 0, num_losses = 0, num_breakeven = 0;
  std::optional<double> win_rate;
  std::optional<double> avg_win_paise, avg_loss_paise;
  std::optional<double> profit_factor;  // null when there are no losses (infinite) or no trades
  std::optional<Paise> best_trade_paise, worst_trade_paise;
  std::optional<double> avg_holding_days, avg_holding_bars;
  std::int64_t num_fills = 0;
  std::int64_t num_open_positions = 0;  // symbols whose last fill left a non-zero position
  Paise traded_value_paise = 0;
  std::optional<double> average_equity_paise;
  std::optional<double> turnover, turnover_annual;
  Paise total_fees_paise = 0;
  std::optional<Paise> net_pnl_paise;    // end equity - start equity (realised + unrealised, after fees)
  std::optional<Paise> gross_pnl_paise;  // net + fees
  std::optional<double> fees_pct_gross_pnl;       // null when gross P&L <= 0
  std::optional<double> fees_pct_initial_capital;  // fees / start equity
};

struct BenchmarkMetrics {
  std::string name;
  std::int64_t n_common_dates = 0, n_pairs = 0;
  std::int64_t n_curve_dates_without_benchmark = 0, n_benchmark_dates_not_in_curve = 0;
  std::optional<Date> first_common_date, last_common_date;
  std::optional<double> strategy_return, benchmark_return, excess_return, benchmark_cagr;
  std::optional<double> beta, alpha_annual, correlation;  // need >= 3 pairs and a non-zero benchmark variance
};

struct Analytics {
  AnalyticsConfig config;
  EquityMetrics equity;
  TradeMetrics trades;
  std::optional<BenchmarkMetrics> benchmark;
};

Analytics analyze(const std::vector<DailyRecord>& curve, const std::vector<Trade>& trades, const std::vector<Fill>& fills,
                  const AnalyticsConfig& cfg = {}, const BenchmarkSeries* benchmark = nullptr);

// summary.json content. Dates are "YYYY-MM-DD" strings; undefined values are null.
nlohmann::json to_json(const Analytics& a);

}  // namespace bt
