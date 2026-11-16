#include "bt/analytics.hpp"

#include <algorithm>
#include <cmath>
#include <set>
#include <stdexcept>

namespace bt {
namespace {

constexpr double kCalendarDaysPerYear = 365.25;

double mean_of(const std::vector<double>& v) {
  double s = 0;
  for (double x : v) s += x;
  return s / static_cast<double>(v.size());
}

// Sample standard deviation (n-1), two-pass. Requires v.size() >= 2.
double sample_sd(const std::vector<double>& v, double mean) {
  double ss = 0;
  for (double x : v) ss += (x - mean) * (x - mean);
  return std::sqrt(ss / static_cast<double>(v.size() - 1));
}

double ratio(Paise a, Paise b) { return static_cast<double>(a) / static_cast<double>(b); }

EquityMetrics equity_metrics(const std::vector<DailyRecord>& c, const AnalyticsConfig& cfg) {
  EquityMetrics m;
  const std::size_t n = c.size();
  m.n_records = static_cast<std::int64_t>(n);
  if (n == 0) return m;

  m.start_date = c.front().date;
  m.end_date = c.back().date;
  m.start_equity = c.front().equity;
  m.end_equity = c.back().equity;
  const Paise e0 = c.front().equity, e1 = c.back().equity;
  if (e0 > 0) m.total_return = ratio(e1, e0) - 1.0;
  const double years = static_cast<double>(c.back().date - c.front().date) / kCalendarDaysPerYear;
  if (years > 0) m.years = years;
  if (years > 0 && e0 > 0 && e1 > 0) m.cagr = std::pow(ratio(e1, e0), 1.0 / years) - 1.0;

  // ---- exposure (needs no returns) ----
  std::int64_t invested = 0, positive = 0;
  double exposure_sum = 0;
  bool all_positive = true;
  for (const DailyRecord& r : c) {
    if (r.positions_value != 0) ++invested;
    if (r.equity > 0) {
      ++positive;
      exposure_sum += std::fabs(static_cast<double>(r.positions_value)) / static_cast<double>(r.equity);
    } else {
      all_positive = false;
    }
  }
  m.time_in_market = static_cast<double>(invested) / static_cast<double>(n);
  if (positive > 0) m.avg_exposure = exposure_sum / static_cast<double>(positive);

  // ---- drawdown (needs positive equity to be a ratio) ----
  if (all_positive) {
    Paise peak = c[0].equity;
    std::size_t peak_idx = 0, dd_peak = 0, dd_trough = 0;
    double max_dd = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
      if (c[i].equity > peak) {
        peak = c[i].equity;
        peak_idx = i;
      }
      const double dd = ratio(c[i].equity, peak) - 1.0;
      if (dd < max_dd) {  // strict: first occurrence of the minimum wins ties
        max_dd = dd;
        dd_peak = peak_idx;
        dd_trough = i;
      }
    }
    m.max_drawdown = max_dd;
    if (max_dd < 0.0) {
      m.max_drawdown_peak_date = c[dd_peak].date;
      m.max_drawdown_trough_date = c[dd_trough].date;
      m.max_drawdown_recovered = false;
      Date end = c.back().date;
      for (std::size_t j = dd_trough + 1; j < n; ++j)
        if (c[j].equity >= c[dd_peak].equity) {
          m.max_drawdown_recovery_date = c[j].date;
          m.max_drawdown_recovered = true;
          end = c[j].date;
          break;
        }
      m.max_drawdown_duration_days = end - c[dd_peak].date;
    } else {
      m.max_drawdown_duration_days = 0;
    }
    // Longest underwater spell: from a peak date until the first date with equity back at/above that peak.
    std::int64_t longest = 0;
    Paise pk = c[0].equity;
    std::size_t pk_idx = 0;
    bool under = false;
    for (std::size_t i = 1; i < n; ++i) {
      if (c[i].equity >= pk) {
        if (under) {
          longest = std::max<std::int64_t>(longest, c[i].date - c[pk_idx].date);
          under = false;
        }
        if (c[i].equity > pk) {
          pk = c[i].equity;
          pk_idx = i;
        }
      } else {
        under = true;
      }
    }
    if (under) longest = std::max<std::int64_t>(longest, c.back().date - c[pk_idx].date);
    m.longest_underwater_days = longest;
    if (m.cagr && max_dd < 0.0) m.calmar = *m.cagr / std::fabs(max_dd);
  }

  // ---- return-based statistics ----
  if (n >= 2 && all_positive) {
    std::vector<double> r;
    r.reserve(n - 1);
    for (std::size_t i = 1; i < n; ++i) r.push_back(ratio(c[i].equity, c[i - 1].equity) - 1.0);
    const double mean = mean_of(r);
    m.mean_daily_return = mean;
    // best / worst day: first occurrence on ties; the date is that of the later record of the pair
    std::size_t bi = 0, wi = 0;
    for (std::size_t i = 1; i < r.size(); ++i) {
      if (r[i] > r[bi]) bi = i;
      if (r[i] < r[wi]) wi = i;
    }
    m.best_day = r[bi];
    m.best_day_date = c[bi + 1].date;
    m.worst_day = r[wi];
    m.worst_day_date = c[wi + 1].date;

    const double td = static_cast<double>(cfg.trading_days);
    const double rf_d = cfg.risk_free_annual / td;
    const double mean_excess = mean - rf_d;
    double down2 = 0;
    for (double x : r) {
      const double e = std::min(x - rf_d, 0.0);
      down2 += e * e;
    }
    const double dd_dev = std::sqrt(down2 / static_cast<double>(r.size()));
    if (dd_dev > 0.0) m.sortino = mean_excess / dd_dev * std::sqrt(td);
    if (r.size() >= 2) {
      const double sd = sample_sd(r, mean);
      m.daily_return_sd = sd;
      m.annualised_volatility = sd * std::sqrt(td);
      if (sd > 0.0) m.sharpe = mean_excess / sd * std::sqrt(td);
    }
  }
  return m;
}

TradeMetrics trade_metrics(const std::vector<DailyRecord>& curve, const std::vector<Trade>& trades,
                           const std::vector<Fill>& fills, const EquityMetrics& eq) {
  TradeMetrics m;
  m.num_trades = static_cast<std::int64_t>(trades.size());
  double win_sum = 0, loss_sum = 0, days = 0, bars = 0;
  Paise best = 0, worst = 0;
  for (std::size_t i = 0; i < trades.size(); ++i) {
    const Paise p = trades[i].net_pnl;
    if (p > 0) {
      ++m.num_wins;
      win_sum += static_cast<double>(p);
    } else if (p < 0) {
      ++m.num_losses;
      loss_sum += static_cast<double>(p);
    } else {
      ++m.num_breakeven;
    }
    if (i == 0 || p > best) best = p;
    if (i == 0 || p < worst) worst = p;
    days += static_cast<double>(trades[i].exit_date - trades[i].entry_date);
    bars += static_cast<double>(trades[i].exit_day) - static_cast<double>(trades[i].entry_day);
  }
  if (m.num_trades > 0) {
    const double nt = static_cast<double>(m.num_trades);
    m.win_rate = static_cast<double>(m.num_wins) / nt;
    m.best_trade_paise = best;
    m.worst_trade_paise = worst;
    m.avg_holding_days = days / nt;
    m.avg_holding_bars = bars / nt;
  }
  if (m.num_wins > 0) m.avg_win_paise = win_sum / static_cast<double>(m.num_wins);
  if (m.num_losses > 0) {
    m.avg_loss_paise = loss_sum / static_cast<double>(m.num_losses);
    m.profit_factor = win_sum / std::fabs(loss_sum);
  }

  // fills: turnover, fees, open positions
  m.num_fills = static_cast<std::int64_t>(fills.size());
  std::vector<std::pair<std::size_t, std::int64_t>> last_pos;  // symbol -> position after its last fill
  for (const Fill& f : fills) {
    m.traded_value_paise += f.notional;
    m.total_fees_paise += f.fees.total();
    bool found = false;
    for (auto& lp : last_pos)
      if (lp.first == f.symbol) { lp.second = f.position_after; found = true; }
    if (!found) last_pos.emplace_back(f.symbol, f.position_after);
  }
  for (const auto& lp : last_pos)
    if (lp.second != 0) ++m.num_open_positions;

  if (!curve.empty()) {
    double s = 0;
    for (const DailyRecord& r : curve) s += static_cast<double>(r.equity);
    m.average_equity_paise = s / static_cast<double>(curve.size());
    if (*m.average_equity_paise > 0) {
      m.turnover = static_cast<double>(m.traded_value_paise) / (2.0 * *m.average_equity_paise);
      if (eq.years) m.turnover_annual = *m.turnover / *eq.years;
    }
    m.net_pnl_paise = eq.end_equity.value() - eq.start_equity.value();
    m.gross_pnl_paise = *m.net_pnl_paise + m.total_fees_paise;
    if (*m.gross_pnl_paise > 0)
      m.fees_pct_gross_pnl = static_cast<double>(m.total_fees_paise) / static_cast<double>(*m.gross_pnl_paise);
    if (eq.start_equity.value() > 0)
      m.fees_pct_initial_capital = static_cast<double>(m.total_fees_paise) / static_cast<double>(eq.start_equity.value());
  }
  return m;
}

BenchmarkMetrics benchmark_metrics(const std::vector<DailyRecord>& curve, const BenchmarkSeries& b,
                                   const AnalyticsConfig& cfg) {
  if (b.dates.size() != b.values.size()) throw std::invalid_argument("benchmark: dates and values differ in length");
  for (std::size_t i = 0; i < b.dates.size(); ++i) {
    if (!(b.values[i] > 0.0) || !std::isfinite(b.values[i])) throw std::invalid_argument("benchmark: values must be finite and > 0");
    if (i > 0 && b.dates[i] <= b.dates[i - 1]) throw std::invalid_argument("benchmark: dates must be strictly increasing");
  }
  BenchmarkMetrics m;
  m.name = b.name;

  // Common dates by merging the two sorted date lists.
  std::vector<std::size_t> ci, bi;
  {
    std::size_t i = 0, j = 0;
    while (i < curve.size() && j < b.dates.size()) {
      if (curve[i].date == b.dates[j]) { ci.push_back(i); bi.push_back(j); ++i; ++j; }
      else if (curve[i].date < b.dates[j]) ++i;
      else ++j;
    }
  }
  const std::size_t nc = ci.size();
  m.n_common_dates = static_cast<std::int64_t>(nc);
  m.n_pairs = nc > 0 ? static_cast<std::int64_t>(nc - 1) : 0;
  m.n_curve_dates_without_benchmark = static_cast<std::int64_t>(curve.size() - nc);
  m.n_benchmark_dates_not_in_curve = static_cast<std::int64_t>(b.dates.size() - nc);
  if (nc == 0) return m;
  m.first_common_date = curve[ci.front()].date;
  m.last_common_date = curve[ci.back()].date;
  if (nc < 2) return m;

  const Paise s0 = curve[ci.front()].equity, s1 = curve[ci.back()].equity;
  bool positive = true;
  for (std::size_t k : ci) positive = positive && curve[k].equity > 0;
  if (!positive || s0 <= 0) return m;
  const double bench0 = b.values[bi.front()], bench1 = b.values[bi.back()];
  m.strategy_return = ratio(s1, s0) - 1.0;
  m.benchmark_return = bench1 / bench0 - 1.0;
  m.excess_return = *m.strategy_return - *m.benchmark_return;
  const double years = static_cast<double>(*m.last_common_date - *m.first_common_date) / kCalendarDaysPerYear;
  if (years > 0 && s1 > 0) m.benchmark_cagr = std::pow(bench1 / bench0, 1.0 / years) - 1.0;

  if (nc - 1 < 3) return m;  // too few pairs for a covariance worth reporting
  std::vector<double> rs, rb;
  for (std::size_t k = 1; k < nc; ++k) {
    rs.push_back(ratio(curve[ci[k]].equity, curve[ci[k - 1]].equity) - 1.0);
    rb.push_back(b.values[bi[k]] / b.values[bi[k - 1]] - 1.0);
  }
  const double ms = mean_of(rs), mb = mean_of(rb);
  double cov = 0, var_b = 0;
  for (std::size_t k = 0; k < rs.size(); ++k) {
    cov += (rs[k] - ms) * (rb[k] - mb);
    var_b += (rb[k] - mb) * (rb[k] - mb);
  }
  const double denom = static_cast<double>(rs.size() - 1);
  cov /= denom;
  var_b /= denom;
  if (var_b > 0.0) {
    const double beta = cov / var_b;
    m.beta = beta;
    const double td = static_cast<double>(cfg.trading_days);
    const double rf_d = cfg.risk_free_annual / td;
    m.alpha_annual = ((ms - rf_d) - beta * (mb - rf_d)) * td;
    const double sd_s = sample_sd(rs, ms), sd_b = std::sqrt(var_b);
    if (sd_s > 0.0) m.correlation = cov / (sd_s * sd_b);
  }
  return m;
}

}  // namespace

BenchmarkSeries benchmark_from_bars(const std::string& name, const std::vector<Bar>& bars) {
  BenchmarkSeries b;
  b.name = name;
  for (const Bar& x : bars) {
    b.dates.push_back(x.date);
    b.values.push_back(x.close);
  }
  return b;
}

Analytics analyze(const std::vector<DailyRecord>& curve, const std::vector<Trade>& trades, const std::vector<Fill>& fills,
                  const AnalyticsConfig& cfg, const BenchmarkSeries* benchmark) {
  if (cfg.trading_days <= 0 || !(cfg.risk_free_annual > -1.0)) throw std::invalid_argument("analytics: bad config");
  for (std::size_t i = 1; i < curve.size(); ++i)
    if (curve[i].date <= curve[i - 1].date) throw std::invalid_argument("analytics: equity curve dates must be strictly increasing");
  Analytics a;
  a.config = cfg;
  a.equity = equity_metrics(curve, cfg);
  a.trades = trade_metrics(curve, trades, fills, a.equity);
  if (benchmark) a.benchmark = benchmark_metrics(curve, *benchmark, cfg);
  return a;
}

// ------------------------------------------------------------------ JSON
namespace {
using nlohmann::json;

json date_json(const std::optional<Date>& d) { return d ? json(format_date(*d)) : json(nullptr); }
template <class T>
json opt_json(const std::optional<T>& v) {
  return v ? json(*v) : json(nullptr);
}
}  // namespace

json to_json(const Analytics& a) {
  json j;
  j["config"] = {{"risk_free_annual", a.config.risk_free_annual},
                 {"trading_days_per_year", a.config.trading_days},
                 {"calendar_days_per_year", kCalendarDaysPerYear}};
  const EquityMetrics& e = a.equity;
  j["equity"] = {{"n_records", e.n_records},
                 {"start_date", date_json(e.start_date)},
                 {"end_date", date_json(e.end_date)},
                 {"start_equity_paise", opt_json(e.start_equity)},
                 {"end_equity_paise", opt_json(e.end_equity)},
                 {"total_return", opt_json(e.total_return)},
                 {"years", opt_json(e.years)},
                 {"cagr", opt_json(e.cagr)},
                 {"mean_daily_return", opt_json(e.mean_daily_return)},
                 {"daily_return_sd", opt_json(e.daily_return_sd)},
                 {"annualised_volatility", opt_json(e.annualised_volatility)},
                 {"sharpe", opt_json(e.sharpe)},
                 {"sortino", opt_json(e.sortino)},
                 {"max_drawdown", opt_json(e.max_drawdown)},
                 {"max_drawdown_peak_date", date_json(e.max_drawdown_peak_date)},
                 {"max_drawdown_trough_date", date_json(e.max_drawdown_trough_date)},
                 {"max_drawdown_recovery_date", date_json(e.max_drawdown_recovery_date)},
                 {"max_drawdown_recovered", opt_json(e.max_drawdown_recovered)},
                 {"max_drawdown_duration_days", opt_json(e.max_drawdown_duration_days)},
                 {"longest_underwater_days", opt_json(e.longest_underwater_days)},
                 {"calmar", opt_json(e.calmar)},
                 {"best_day", opt_json(e.best_day)},
                 {"best_day_date", date_json(e.best_day_date)},
                 {"worst_day", opt_json(e.worst_day)},
                 {"worst_day_date", date_json(e.worst_day_date)},
                 {"time_in_market", opt_json(e.time_in_market)},
                 {"avg_exposure", opt_json(e.avg_exposure)}};
  const TradeMetrics& t = a.trades;
  j["trades"] = {{"num_trades", t.num_trades},
                 {"num_wins", t.num_wins},
                 {"num_losses", t.num_losses},
                 {"num_breakeven", t.num_breakeven},
                 {"win_rate", opt_json(t.win_rate)},
                 {"avg_win_paise", opt_json(t.avg_win_paise)},
                 {"avg_loss_paise", opt_json(t.avg_loss_paise)},
                 {"profit_factor", opt_json(t.profit_factor)},
                 {"best_trade_paise", opt_json(t.best_trade_paise)},
                 {"worst_trade_paise", opt_json(t.worst_trade_paise)},
                 {"avg_holding_days", opt_json(t.avg_holding_days)},
                 {"avg_holding_bars", opt_json(t.avg_holding_bars)},
                 {"num_fills", t.num_fills},
                 {"num_open_positions", t.num_open_positions},
                 {"traded_value_paise", t.traded_value_paise},
                 {"average_equity_paise", opt_json(t.average_equity_paise)},
                 {"turnover", opt_json(t.turnover)},
                 {"turnover_annual", opt_json(t.turnover_annual)},
                 {"total_fees_paise", t.total_fees_paise},
                 {"net_pnl_paise", opt_json(t.net_pnl_paise)},
                 {"gross_pnl_paise", opt_json(t.gross_pnl_paise)},
                 {"fees_pct_gross_pnl", opt_json(t.fees_pct_gross_pnl)},
                 {"fees_pct_initial_capital", opt_json(t.fees_pct_initial_capital)}};
  if (a.benchmark) {
    const BenchmarkMetrics& b = *a.benchmark;
    j["benchmark"] = {{"name", b.name},
                      {"n_common_dates", b.n_common_dates},
                      {"n_pairs", b.n_pairs},
                      {"n_curve_dates_without_benchmark", b.n_curve_dates_without_benchmark},
                      {"n_benchmark_dates_not_in_curve", b.n_benchmark_dates_not_in_curve},
                      {"first_common_date", date_json(b.first_common_date)},
                      {"last_common_date", date_json(b.last_common_date)},
                      {"strategy_return", opt_json(b.strategy_return)},
                      {"benchmark_return", opt_json(b.benchmark_return)},
                      {"excess_return", opt_json(b.excess_return)},
                      {"benchmark_cagr", opt_json(b.benchmark_cagr)},
                      {"beta", opt_json(b.beta)},
                      {"alpha_annual", opt_json(b.alpha_annual)},
                      {"correlation", opt_json(b.correlation)}};
  } else {
    j["benchmark"] = nullptr;
  }
  return j;
}

}  // namespace bt
