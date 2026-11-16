#include "bt/strategies.hpp"

#include <algorithm>
#include <cmath>
#include <set>

#include "bt/indicators.hpp"
#include "bt/json_util.hpp"
#include "bt/sizing.hpp"

namespace bt {
namespace {

// Shared plumbing for strategies that trade ONE symbol.
class SymbolStrategy : public Strategy {
 public:
  SymbolStrategy(std::string symbol, double allocation) : symbol_(std::move(symbol)), allocation_(allocation) {}

  void on_start(Context& c) override {
    if (c.num_symbols() == 0) throw ConfigError("strategy: the data contains no symbols");
    if (symbol_.empty()) {
      sym_ = 0;
    } else {
      const auto i = c.symbol_index(symbol_);
      if (!i) throw ConfigError("strategy: symbol '" + symbol_ + "' is not in the loaded data");
      sym_ = *i;
    }
  }

  void on_bar(Context& c) final {
    const HistoryView h = c.history(sym_);
    if (h.empty()) return;              // not listed yet
    const Bar& b = h.latest();
    if (have_last_ && b.date == last_date_) return;  // no NEW bar for this symbol today (hole)
    have_last_ = true;
    last_date_ = b.date;
    on_new_bar(c, b);
  }

 protected:
  virtual void on_new_bar(Context& c, const Bar& bar) = 0;

  // Shares affordable with `allocation` of current equity at `price`; 0 means "do not trade".
  std::int64_t entry_qty(const Context& c, double price) const {
    return qty_for_amount(c.equity() * allocation_, price);
  }

  std::size_t sym_ = 0;

 private:
  std::string symbol_;
  double allocation_;
  bool have_last_ = false;
  Date last_date_ = 0;
};

// ---------------------------------------------------------------- BuyAndHold
class BuyAndHold final : public SymbolStrategy {
 public:
  using SymbolStrategy::SymbolStrategy;

 protected:
  void on_new_bar(Context& c, const Bar& bar) override {
    if (c.position(sym_) != 0) return;  // already holding: never sell
    const std::int64_t q = entry_qty(c, bar.close);
    if (q > 0) c.buy(sym_, q);  // if rejected/expired, simply tried again on the next bar
  }
};

// ---------------------------------------------------------------- SmaCrossover
class SmaCrossover final : public SymbolStrategy {
 public:
  SmaCrossover(std::string symbol, double allocation, std::size_t fast, std::size_t slow)
      : SymbolStrategy(std::move(symbol), allocation), fast_(fast), slow_(slow) {}

 protected:
  void on_new_bar(Context& c, const Bar& bar) override {
    fast_.update(bar.close);
    slow_.update(bar.close);
    if (!slow_.ready()) return;  // warm-up (fast has been ready for longer)
    const std::int64_t pos = c.position(sym_);
    if (fast_.value() > slow_.value()) {  // strictly above => long; EQUAL counts as not above
      if (pos == 0) {
        const std::int64_t q = entry_qty(c, bar.close);
        if (q > 0) c.buy(sym_, q);
      }
    } else if (pos > 0) {
      c.sell(sym_, pos);
    }
  }

 private:
  Sma fast_, slow_;
};

// ---------------------------------------------------------------- RsiMeanReversion
class RsiMeanReversion final : public SymbolStrategy {
 public:
  RsiMeanReversion(std::string symbol, double allocation, std::size_t period, double buy_below, double sell_above)
      : SymbolStrategy(std::move(symbol), allocation), rsi_(period), buy_below_(buy_below), sell_above_(sell_above) {}

 protected:
  void on_new_bar(Context& c, const Bar& bar) override {
    rsi_.update(bar.close);
    if (!rsi_.ready()) return;
    const double r = rsi_.value();
    const std::int64_t pos = c.position(sym_);
    if (pos == 0 && r < buy_below_) {  // strict: RSI exactly at the threshold does nothing
      const std::int64_t q = entry_qty(c, bar.close);
      if (q > 0) c.buy(sym_, q);
    } else if (pos > 0 && r > sell_above_) {
      c.sell(sym_, pos);
    }
  }

 private:
  Rsi rsi_;
  double buy_below_, sell_above_;
};

// ---------------------------------------------------------------- DonchianBreakout
class DonchianBreakout final : public SymbolStrategy {
 public:
  DonchianBreakout(std::string symbol, double allocation, std::size_t entry_n, std::size_t exit_n,
                   std::size_t atr_period, double atr_mult)
      : SymbolStrategy(std::move(symbol), allocation),
        upper_(entry_n), lower_(exit_n), atr_(atr_period), atr_mult_(atr_mult) {}

 protected:
  void on_new_bar(Context& c, const Bar& bar) override {
    // Channels cover the n bars BEFORE today: read them first, then fold today's bar in.
    const bool have_up = upper_.ready(), have_lo = lower_.ready();
    const double up = upper_.value(), lo = lower_.value();
    upper_.update(bar.high);
    lower_.update(bar.low);
    atr_.update(bar.high, bar.low, bar.close);

    const std::int64_t pos = c.position(sym_);
    reconcile_stop(c, pos);

    if (pos == 0) {
      // Entry: close strictly above the highest high of the previous entry_n bars.
      if (have_up && atr_.ready() && bar.close > up) {
        const std::int64_t q = entry_qty(c, bar.close);
        if (q <= 0) return;
        const double stop = bar.close - atr_mult_ * atr_.value();
        c.buy(sym_, q);
        if (to_paise(stop) >= 1) {  // a stop that would be <= 0.00 is skipped rather than risking an invalid order
          stop_p_ = to_paise(stop);
          c.stop(sym_, Side::Sell, q, to_rupees(stop_p_), TimeInForce::GTC);  // protects the first bar after the fill
        }
      }
    } else if (have_lo && bar.close < lo) {
      // Exit: close strictly below the lowest low of the previous exit_n bars. Drop the stop first. stop_p_ is kept
      // on purpose: if this Day exit expires unfilled (no bar tomorrow), the next bar's reconcile_stop() re-arms it.
      cancel_stops(c);
      c.sell(sym_, pos);
    }
  }

 private:
  void cancel_stops(Context& c) {
    for (const Order& o : c.open_orders(sym_))
      if (o.type == OrderType::Stop && o.side == Side::Sell) c.cancel(o.id);
  }

  // Keep exactly one stop, at the level chosen at entry and sized to the CURRENT position:
  //   flat -> remove leftovers (entry was rejected / stop already hit);
  //   long -> if the live stop has the wrong size (e.g. partial fill) or level, replace it.
  void reconcile_stop(Context& c, std::int64_t pos) {
    if (pos <= 0) {
      cancel_stops(c);
      stop_p_ = 0;
      return;
    }
    if (stop_p_ < 1) return;  // entered without a stop (stop level was not positive)
    bool ok = false;
    for (const Order& o : c.open_orders(sym_)) {
      if (o.type != OrderType::Stop || o.side != Side::Sell) continue;
      if (!ok && o.price == stop_p_ && o.remaining == pos) ok = true;
      else c.cancel(o.id);
    }
    if (!ok) c.stop(sym_, Side::Sell, pos, to_rupees(stop_p_), TimeInForce::GTC);
  }

  RollingMax upper_;
  RollingMin lower_;
  Atr atr_;
  double atr_mult_;
  Paise stop_p_ = 0;  // protective stop level in paise (0 = none)
};

// ---------------------------------------------------------------- MomentumRotation
class MomentumRotation final : public Strategy {
 public:
  MomentumRotation(std::vector<std::string> universe, std::size_t lookback, std::size_t top_k, double allocation,
                   int max_stale_days, bool rebalance_if_unchanged)
      : names_(std::move(universe)), lookback_(lookback), top_k_(top_k), allocation_(allocation),
        max_stale_days_(max_stale_days), always_(rebalance_if_unchanged) {}

  void on_start(Context& c) override {
    if (names_.empty()) {
      for (std::size_t s = 0; s < c.num_symbols(); ++s) universe_.push_back(s);
    } else {
      std::set<std::size_t> seen;
      for (const std::string& n : names_) {
        const auto i = c.symbol_index(n);
        if (!i) throw ConfigError("strategy: universe symbol '" + n + "' is not in the loaded data");
        if (!seen.insert(*i).second) throw ConfigError("strategy: universe lists '" + n + "' twice");
        universe_.push_back(*i);
      }
    }
    if (top_k_ > universe_.size())
      throw ConfigError("strategy: top_k (" + std::to_string(top_k_) + ") exceeds the universe size (" +
                        std::to_string(universe_.size()) + ")");
  }

  void on_bar(Context& c) override {
    // Momentum keeps no incremental indicator state (it reads the history directly) and a rebalance made in warm-up would be
    // discarded while still using up the month, so it simply does nothing until the evaluation window starts.
    if (c.in_warmup()) return;
    // First bar of a new calendar month, judged from the visible date alone.
    const CivilDate d = civil_from_days(c.date());
    const int ym = d.year * 12 + (d.month - 1);
    if (ym == last_ym_) return;

    // Rank by N-BAR return: close today / close `lookback` bars ago - 1, using the symbol's own bars.
    struct Cand { double ret; std::size_t sym; };
    std::vector<Cand> cands;
    for (std::size_t s : universe_) {
      const HistoryView h = c.history(s);
      if (h.size() <= lookback_) continue;                                 // not enough history yet
      if (c.date() - h.latest().date > max_stale_days_) continue;          // stopped trading: do not rank on stale data
      cands.push_back({h[0].close / h[lookback_].close - 1.0, s});
    }
    if (cands.empty()) return;  // nothing rankable: do NOT consume this month, try again tomorrow
    last_ym_ = ym;

    std::sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) {
      if (a.ret != b.ret) return a.ret > b.ret;
      return a.sym < b.sym;  // deterministic tie-break: lower symbol index first
    });
    const std::size_t k = std::min(top_k_, cands.size());

    std::set<std::size_t> selected, held;
    for (std::size_t i = 0; i < k; ++i) selected.insert(cands[i].sym);
    for (std::size_t s : universe_)
      if (c.position(s) > 0) held.insert(s);
    if (!always_ && selected == held) return;  // same names already held: avoid churn (and its costs)

    std::vector<TargetWeight> targets;
    for (std::size_t s : selected) targets.push_back({s, 1.0 / static_cast<double>(top_k_)});
    rebalance_to_weights(c, targets, (1.0 - allocation_) * 100.0);
  }

 private:
  std::vector<std::string> names_;
  std::vector<std::size_t> universe_;
  std::size_t lookback_, top_k_;
  double allocation_;
  int max_stale_days_;
  bool always_;
  int last_ym_ = -1;
};

// ---------------------------------------------------------------- factory
double get_allocation(const nlohmann::json& p, double def = 0.95) {
  const double a = cfg::get_num(p, "allocation", def, "params", 0.0, 1.0);
  if (!(a > 0.0)) throw ConfigError("params.allocation: must be > 0 and <= 1");
  return a;
}

std::size_t get_period(const nlohmann::json& p, const char* key, long long def) {
  return static_cast<std::size_t>(cfg::get_int(p, key, def, "params", 1, 100000));
}

}  // namespace

const std::vector<std::string>& strategy_names() {
  static const std::vector<std::string> names = {"buy_and_hold", "sma_crossover", "rsi_mean_reversion",
                                                 "donchian_breakout", "momentum_rotation"};
  return names;
}

std::unique_ptr<Strategy> make_strategy(const std::string& name, const nlohmann::json& params) {
  const nlohmann::json empty = nlohmann::json::object();
  const nlohmann::json& p = params.is_null() ? empty : params;
  cfg::expect_object(p, "params");

  if (name == "buy_and_hold") {
    cfg::check_keys(p, {"symbol", "allocation"}, "params");
    // 0.99, not 0.95: the benchmark should not be handicapped by 5 % of cash drag. The 1 % headroom still covers the
    // buy-side fees; a larger gap up is rejected and retried (re-sized) on the next bar.
    return std::make_unique<BuyAndHold>(cfg::get_str(p, "symbol", "", "params"), get_allocation(p, 0.99));
  }
  if (name == "sma_crossover") {
    cfg::check_keys(p, {"symbol", "allocation", "fast", "slow"}, "params");
    const std::size_t fast = get_period(p, "fast", 20), slow = get_period(p, "slow", 50);
    if (fast >= slow) throw ConfigError("params: fast (" + std::to_string(fast) + ") must be less than slow (" + std::to_string(slow) + ")");
    return std::make_unique<SmaCrossover>(cfg::get_str(p, "symbol", "", "params"), get_allocation(p), fast, slow);
  }
  if (name == "rsi_mean_reversion") {
    cfg::check_keys(p, {"symbol", "allocation", "period", "buy_below", "sell_above"}, "params");
    const double lo = cfg::get_num(p, "buy_below", 30.0, "params", 0.0, 100.0);
    const double hi = cfg::get_num(p, "sell_above", 70.0, "params", 0.0, 100.0);
    if (!(lo < hi)) throw ConfigError("params: buy_below must be less than sell_above");
    return std::make_unique<RsiMeanReversion>(cfg::get_str(p, "symbol", "", "params"), get_allocation(p),
                                              get_period(p, "period", 14), lo, hi);
  }
  if (name == "donchian_breakout") {
    cfg::check_keys(p, {"symbol", "allocation", "entry_n", "exit_n", "atr_period", "atr_mult"}, "params");
    const double mult = cfg::get_num(p, "atr_mult", 2.0, "params", 0.0, 1000.0);
    if (!(mult > 0.0)) throw ConfigError("params.atr_mult: must be > 0");
    return std::make_unique<DonchianBreakout>(cfg::get_str(p, "symbol", "", "params"), get_allocation(p),
                                              get_period(p, "entry_n", 20), get_period(p, "exit_n", 10),
                                              get_period(p, "atr_period", 14), mult);
  }
  if (name == "momentum_rotation") {
    cfg::check_keys(p, {"universe", "lookback", "top_k", "allocation", "max_stale_days", "rebalance_if_unchanged"}, "params");
    return std::make_unique<MomentumRotation>(
        cfg::get_str_list(p, "universe", "params"), get_period(p, "lookback", 126), get_period(p, "top_k", 3),
        get_allocation(p), static_cast<int>(cfg::get_int(p, "max_stale_days", 7, "params", 0, 3650)),
        cfg::get_bool(p, "rebalance_if_unchanged", false, "params"));
  }
  throw ConfigError("unknown strategy '" + name + "' (known: buy_and_hold, sma_crossover, rsi_mean_reversion, donchian_breakout, momentum_rotation)");
}

}  // namespace bt
