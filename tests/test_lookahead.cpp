// The look-ahead test.
//
// Claim: a decision made on day t depends only on data up to and including day t.
// Test: run a strategy on data A, then on data B = A with EVERY bar after day t replaced by random garbage
// (different prices by orders of magnitude, different dates, different holes). Everything the engine and
// strategy produced through day t - decisions, observations, equity, fills, order events - must be identical.
//
// Two positive controls keep this from passing vacuously:
//   1. after day t the two runs DO differ (the garbage really reaches the strategy, just later);
//   2. a deliberately cheating strategy that peeks at the underlying series IS caught by the same harness.
#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <random>

#include "bt/strategies.hpp"
#include "bt/synthetic.hpp"
#include "doctest.h"
#include "test_util.hpp"

using namespace tu;

namespace {

struct Decision {
  std::size_t day;
  std::size_t symbol;
  int kind;
  std::int64_t qty;
  Paise price;
  bool operator==(const Decision& o) const {
    return day == o.day && symbol == o.symbol && kind == o.kind && qty == o.qty && price == o.price;
  }
};

std::uint64_t mix(std::uint64_t h, std::uint64_t v) { return (h ^ v) * 1099511628211ULL; }
std::uint64_t bits(double d) {
  std::uint64_t u;
  std::memcpy(&u, &d, sizeof u);
  return u;
}

// Reads deep history (5/20/30-bar windows plus today's bar) on every symbol, places market, limit and stop
// orders whose size and price depend on what it read, cancels and replaces.
struct Probe : Strategy {
  std::vector<Decision> decisions;
  std::vector<std::uint64_t> obs;  // one fingerprint per day of everything read

  void on_bar(Context& c) override {
    const std::size_t day = c.day_index();
    std::uint64_t fp = 1469598103934665603ULL;
    for (std::size_t s = 0; s < c.num_symbols(); ++s) {
      const HistoryView v = c.history(s);
      fp = mix(fp, v.size());
      if (v.size() < 31) continue;
      double sum5 = 0, sum30 = 0, hi20 = 0, lo20 = 1e300;
      for (std::size_t i = 0; i < 30; ++i) sum30 += v[i].close;
      for (std::size_t i = 0; i < 5; ++i) sum5 += v[i].close / 5.0;
      sum30 /= 30.0;
      for (std::size_t i = 0; i < 20; ++i) {
        hi20 = std::max(hi20, v[i].high);
        lo20 = std::min(lo20, v[i].low);
      }
      const double px = v[0].close;
      const std::int64_t vol0 = v[0].volume;
      fp = mix(mix(mix(mix(fp, bits(sum5)), bits(sum30)), bits(hi20)), bits(lo20));
      fp = mix(fp, static_cast<std::uint64_t>(vol0));
      fp = mix(fp, c.traded_today(s) ? 1 : 0);

      const std::int64_t pos = c.position(s);
      auto rec = [&](int kind, std::int64_t q, double price) {
        decisions.push_back({day, s, kind, q, to_paise(price)});
      };
      if (pos == 0) {
        if (sum5 > sum30) {
          const std::int64_t q = 1 + vol0 % 17;
          c.cancel_all(s);
          rec(4, 0, 0);
          c.buy(s, q);
          rec(0, q, 0);
          c.stop(s, Side::Sell, q, lo20 * 0.99, TimeInForce::GTC);  // protective stop, submitted with the entry
          rec(2, q, lo20 * 0.99);
        } else if (c.open_orders(s).empty()) {
          const std::int64_t q = 1 + vol0 % 11;
          c.limit(s, Side::Buy, q, px * 0.97, TimeInForce::GTC);
          rec(3, q, px * 0.97);
        }
      } else {
        if (sum5 < sum30) {
          c.cancel_all(s);
          rec(4, 0, 0);
          c.sell(s, pos);
          rec(1, pos, 0);
        } else if (c.open_orders(s).size() < 2) {
          c.limit(s, Side::Sell, pos, hi20 * 1.01, TimeInForce::GTC);
          rec(5, pos, hi20 * 1.01);
        }
      }
    }
    obs.push_back(fp);
  }
};

// Peeks at the next real bar of symbol 0 through a back door (the AlignedData it was handed).
struct Cheater : Strategy {
  const AlignedData* data;
  std::vector<Decision> decisions;
  std::vector<std::uint64_t> obs;
  explicit Cheater(const AlignedData* d) : data(d) {}
  void on_bar(Context& c) override {
    const std::size_t vis = c.history(0).size();
    std::uint64_t fp = 7;
    if (vis > 0 && vis < data->bars(0).size()) {
      const double future_close = data->bars(0)[vis].close;  // a bar AFTER today
      const std::int64_t q = 1 + static_cast<std::int64_t>(future_close) % 997;
      c.buy(0, q);
      decisions.push_back({c.day_index(), 0, 0, q, 0});
      fp = mix(fp, bits(future_close));
    }
    obs.push_back(fp);
  }
};

struct Run {
  std::vector<Decision> decisions;
  std::vector<std::uint64_t> obs;
  EngineResult res;
  Date cut_date_unused = 0;
};

EngineConfig test_cfg() {
  EngineConfig c;
  c.initial_cash = to_paise(1'000'000.0);
  c.costs = CostConfig::nse_delivery_defaults();
  c.slippage.mode = SlippageConfig::Mode::FixedBps;
  c.slippage.value = 5;
  return c;
}

Run run_probe(const AlignedData& d) {
  Probe p;
  Run r;
  r.res = Engine(d, test_cfg()).run(p);
  r.decisions = p.decisions;
  r.obs = p.obs;
  return r;
}

Run run_cheater(const AlignedData& d) {
  Cheater p(&d);
  Run r;
  r.res = Engine(d, test_cfg()).run(p);
  r.decisions = p.decisions;
  r.obs = p.obs;
  return r;
}

// Everything produced through calendar day t (inclusive) must be identical.
// curve_offset = number of warm-up days: the equity curve then starts at calendar day `curve_offset`, so calendar day i is
// curve record i - curve_offset (fills and events carry absolute calendar indices / dates already).
bool prefix_equal(const Run& a, const Run& b, std::size_t t, std::size_t curve_offset = 0) {
  const bool compare_obs = !a.obs.empty() || !b.obs.empty();  // built-in strategies record no observations
  for (std::size_t i = 0; i <= t; ++i) {
    if (compare_obs && (i >= a.obs.size() || i >= b.obs.size() || a.obs[i] != b.obs[i])) return false;
    if (i < curve_offset) continue;
    const DailyRecord &x = a.res.equity_curve[i - curve_offset], &y = b.res.equity_curve[i - curve_offset];
    if (x.date != y.date || x.cash != y.cash || x.positions_value != y.positions_value || x.equity != y.equity) return false;
  }
  auto take = [&](const Run& r) {
    std::vector<Decision> v;
    for (const auto& d : r.decisions) if (d.day <= t) v.push_back(d);
    return v;
  };
  if (!(take(a) == take(b))) return false;

  auto fills = [&](const Run& r) {
    std::vector<Fill> v;
    for (const auto& f : r.res.fills) if (f.day_index <= t) v.push_back(f);
    return v;
  };
  const auto fa = fills(a), fb = fills(b);
  if (fa.size() != fb.size()) return false;
  for (std::size_t i = 0; i < fa.size(); ++i) {
    const Fill &x = fa[i], &y = fb[i];
    if (x.order_id != y.order_id || x.date != y.date || x.symbol != y.symbol || x.side != y.side || x.qty != y.qty ||
        x.price != y.price || x.notional != y.notional || !(x.fees == y.fees) || x.cash_after != y.cash_after ||
        x.position_after != y.position_after)
      return false;
  }
  const Date last = a.res.equity_curve[t - curve_offset].date;
  auto events = [&](const Run& r) {
    std::vector<OrderEvent> v;
    for (const auto& e : r.res.events) if (e.date <= last) v.push_back(e);
    return v;
  };
  const auto ea = events(a), eb = events(b);
  if (ea.size() != eb.size()) return false;
  for (std::size_t i = 0; i < ea.size(); ++i)
    if (ea[i].id != eb[i].id || ea[i].date != eb[i].date || ea[i].symbol != eb[i].symbol || ea[i].kind != eb[i].kind ||
        (ea[i].kind == OrderEventKind::Rejected && ea[i].reason != eb[i].reason))
      return false;
  return true;
}

// Do the runs differ anywhere AFTER day t?
bool differs_after(const Run& a, const Run& b, std::size_t t) {
  const std::size_t n = std::min(a.res.equity_curve.size(), b.res.equity_curve.size());
  for (std::size_t i = t + 1; i < n; ++i)
    if (a.res.equity_curve[i].equity != b.res.equity_curve[i].equity) return true;
  for (std::size_t i = t + 1; i < std::min(a.obs.size(), b.obs.size()); ++i)
    if (a.obs[i] != b.obs[i]) return true;
  return false;
}

// ---- data ----
std::vector<SymbolSeries> base_series(std::uint64_t seed) {
  SynthParams p;
  p.start = parse_date("2022-01-03").value();
  p.num_bars = 400;
  p.seed = seed * 10 + 1;
  p.start_price = 120;
  auto a = generate_gbm(p);
  p.seed = seed * 10 + 2;
  p.start_price = 640;
  auto b0 = generate_gbm(p);
  std::vector<Bar> b;  // calendar hole: indices 150..157 missing, plus every 11th bar
  for (std::size_t i = 0; i < b0.size(); ++i)
    if (!(i >= 150 && i < 158) && i % 11 != 5) b.push_back(b0[i]);
  p.seed = seed * 10 + 3;
  p.start_price = 2400;
  auto c0 = generate_gbm(p);
  std::vector<Bar> c(c0.begin() + 60, c0.end());  // lists on day 60
  return {{"A", a}, {"B", b}, {"C", c}};
}

std::vector<Bar> garbage_bars(Date after, std::size_t n, std::uint64_t seed) {
  std::mt19937_64 rng(seed);
  auto U = [&] { return static_cast<double>(rng() >> 11) / 9007199254740992.0; };
  auto cents = [](double x) { return std::max(0.01, std::round(x * 100.0) / 100.0); };
  std::vector<Bar> out;
  Date d = after;
  for (std::size_t i = 0; i < n; ++i) {
    d += 1 + static_cast<Date>(rng() % 3);  // irregular spacing => different holes and calendar
    const double base = std::exp(U() * std::log(10000.0));  // anywhere from 1 to 10,000
    Bar b;
    b.date = d;
    b.open = cents(base * (0.9 + 0.2 * U()));
    b.close = cents(base * (0.9 + 0.2 * U()));
    b.high = std::max({cents(std::max(b.open, b.close) * (1 + 0.2 * U())), b.open, b.close});
    b.low = std::min({cents(std::min(b.open, b.close) * (1 - 0.2 * U())), b.open, b.close});
    b.adj_close = b.close;
    b.volume = static_cast<std::int64_t>(rng() % 5'000'000);  // includes tiny / zero volumes
    out.push_back(b);
  }
  return out;
}

// Same history through `cut`, garbage afterwards.
AlignedData with_garbage_after(const std::vector<SymbolSeries>& base, Date cut, std::uint64_t seed) {
  std::vector<SymbolSeries> out;
  std::uint64_t k = 0;
  for (const SymbolSeries& s : base) {
    SymbolSeries g{s.symbol, {}};
    std::size_t later = 0;
    for (const Bar& b : s.bars) {
      if (b.date <= cut) g.bars.push_back(b);
      else ++later;
    }
    for (const Bar& b : garbage_bars(cut, later, seed * 1000 + (++k))) g.bars.push_back(b);
    out.push_back(std::move(g));
  }
  return align_calendar(std::move(out));
}

}  // namespace

TEST_CASE("look-ahead: decisions through day t are identical when everything after t is garbage") {
  std::size_t checked = 0, cases_with_fills_before_t = 0;
  for (std::uint64_t seed = 1; seed <= 3; ++seed) {
    CAPTURE(seed);
    const std::vector<SymbolSeries> base = base_series(seed);
    const AlignedData a = align_calendar(base);
    const Run ra = run_probe(a);
    REQUIRE(ra.res.equity_curve.size() == a.num_days());

    // t values include: early, after every symbol is listed, just before / at the start / at the end of B's
    // calendar hole (B's bars 150..157 are absent), and late.
    for (std::size_t t : {45u, 100u, 149u, 150u, 157u, 158u, 250u, 330u}) {
      CAPTURE(t);
      REQUIRE(t + 5 < a.num_days());
      const Date cut = a.date(t);
      const AlignedData b = with_garbage_after(base, cut, seed);
      // Identical calendar through the cut.
      for (std::size_t i = 0; i <= t; ++i) REQUIRE(b.date(i) == a.date(i));
      const Run rb = run_probe(b);

      CHECK(prefix_equal(ra, rb, t));          // THE PROPERTY
      CHECK(differs_after(ra, rb, t));         // positive control 1: the garbage does reach the strategy later
      ++checked;
      bool any_fill = false;
      for (const auto& f : ra.res.fills) if (f.day_index <= t) any_fill = true;
      if (any_fill) ++cases_with_fills_before_t;
    }
  }
  CHECK(checked == 24);
  CHECK(cases_with_fills_before_t >= 12);  // most prefixes contain real trading, so equality is not trivial
}

TEST_CASE("look-ahead: the harness catches a strategy that peeks at the future (positive control 2)") {
  std::size_t caught = 0, total = 0;
  for (std::uint64_t seed = 1; seed <= 3; ++seed) {
    const std::vector<SymbolSeries> base = base_series(seed);
    const AlignedData a = align_calendar(base);
    const Run ra = run_cheater(a);
    for (std::size_t t : {45u, 100u, 149u, 250u}) {
      const AlignedData b = with_garbage_after(base, a.date(t), seed);
      const Run rb = run_cheater(b);
      ++total;
      if (!prefix_equal(ra, rb, t)) ++caught;
    }
  }
  CHECK(total == 12);
  CHECK(caught == total);  // every single cut is detected
}

TEST_CASE("look-ahead: the honest probe strategy actually trades (the test is not vacuous)") {
  const AlignedData a = align_calendar(base_series(1));
  const Run r = run_probe(a);
  CHECK(r.res.fills.size() > 20);
  CHECK(!r.decisions.empty());
  bool market = false, limit = false, stop = false;
  for (const auto& d : r.decisions) {
    market |= d.kind == 0;
    limit |= d.kind == 3 || d.kind == 5;
    stop |= d.kind == 2;
  }
  CHECK(market);
  CHECK(limit);
  CHECK(stop);
}

// ---- every built-in strategy goes through the same harness ----
//
// The built-ins record nothing themselves, and an order decided on day t only FILLS on day t+1 (outside the
// compared prefix). So a plain equity/fills comparison would be nearly blind to a one-bar leak (measured: with a
// deliberate one-bar leak only 3 of 60 checks noticed). The Recorder decorator therefore snapshots each
// strategy's order book (pending + staged) before and after every on_bar and records the INTENT: which orders it
// added (symbol, side, type, tif, quantity, price) and which it removed. Those records are compared through day t,
// exactly like the Probe's decisions.
namespace {
struct Builtin { const char* name; const char* params; };
// B has calendar holes and C lists on day 60, so these exercise the hole / late-listing paths too.
const Builtin kBuiltins[] = {
    {"buy_and_hold", R"({"symbol": "B"})"},
    {"sma_crossover", R"({"symbol": "B", "fast": 5, "slow": 20})"},
    {"rsi_mean_reversion", R"({"symbol": "A", "period": 5, "buy_below": 35, "sell_above": 60})"},
    {"donchian_breakout", R"({"symbol": "C", "entry_n": 10, "exit_n": 5, "atr_period": 5, "atr_mult": 1.5})"},
    {"momentum_rotation", R"({"lookback": 15, "top_k": 2})"},
};

struct Recorder : Strategy {
  std::unique_ptr<Strategy> inner;
  std::vector<Decision> decisions;
  explicit Recorder(std::unique_ptr<Strategy> s) : inner(std::move(s)) {}
  void on_start(Context& c) override { inner->on_start(c); }
  void on_end(Context& c) override { inner->on_end(c); }
  void on_bar(Context& c) override {
    auto snap = [&c] {
      std::vector<Order> v;
      for (std::size_t s = 0; s < c.num_symbols(); ++s)
        for (const Order& o : c.open_orders(s)) v.push_back(o);
      return v;
    };
    auto has = [](const std::vector<Order>& v, OrderId id) {
      for (const Order& o : v)
        if (o.id == id) return true;
      return false;
    };
    auto kind = [](const Order& o, int base) {
      return base + static_cast<int>(o.type) * 10 + (o.side == Side::Sell ? 1 : 0) + (o.tif == TimeInForce::GTC ? 1000 : 0);
    };
    const std::vector<Order> before = snap();
    inner->on_bar(c);
    const std::vector<Order> after = snap();
    for (const Order& o : after)
      if (!has(before, o.id)) decisions.push_back({c.day_index(), o.symbol, kind(o, 100), o.qty, o.price});    // added
    for (const Order& o : before)
      if (!has(after, o.id)) decisions.push_back({c.day_index(), o.symbol, kind(o, 200), o.remaining, o.price});  // removed
  }
};

Run run_builtin(const Builtin& b, const AlignedData& d, std::optional<Date> eval_start = std::nullopt) {
  Recorder rec(make_strategy(b.name, nlohmann::json::parse(b.params)));
  Run r;
  EngineConfig ec = test_cfg();
  ec.eval_start = eval_start;
  r.res = Engine(d, ec).run(rec);
  r.decisions = rec.decisions;
  return r;
}

// Fixed cut points plus up to 6 days on which THIS strategy actually placed orders (so the decision made on the cut
// day itself is non-empty and is the one a one-bar leak would corrupt).
std::vector<std::size_t> cut_days(const Run& ra, std::size_t n_days) {
  std::vector<std::size_t> cuts = {70, 149, 150, 158, 250, 330};
  std::vector<std::size_t> active;
  for (const Decision& d : ra.decisions)
    if (d.day + 5 < n_days && (active.empty() || active.back() != d.day)) active.push_back(d.day);
  const std::size_t take = std::min<std::size_t>(6, active.size());
  for (std::size_t i = 0; i < take; ++i) cuts.push_back(active[i * active.size() / take]);
  std::sort(cuts.begin(), cuts.end());
  cuts.erase(std::unique(cuts.begin(), cuts.end()), cuts.end());
  return cuts;
}
}  // namespace

TEST_CASE("look-ahead: every built-in strategy makes identical decisions, fills and equity through day t when everything after t is garbage") {
  std::size_t checked = 0;
  for (const Builtin& bi : kBuiltins) {
    CAPTURE(bi.name);
    std::size_t traded = 0, decision_days_checked = 0;
    for (std::uint64_t seed = 1; seed <= 2; ++seed) {
      const std::vector<SymbolSeries> base = base_series(seed);
      const AlignedData a = align_calendar(base);
      const Run ra = run_builtin(bi, a);
      traded += ra.res.fills.size();
      for (std::size_t t : cut_days(ra, a.num_days())) {
        CAPTURE(seed);
        CAPTURE(t);
        const AlignedData b = with_garbage_after(base, a.date(t), seed);
        const Run rb = run_builtin(bi, b);
        CHECK(prefix_equal(ra, rb, t));
        ++checked;
        for (const Decision& d : ra.decisions)
          if (d.day == t) { ++decision_days_checked; break; }
      }
    }
    CHECK(traded > 0);                 // the strategy really traded on at least one of the base datasets
    CHECK(decision_days_checked >= 2); // and some cuts fall exactly on a day it placed or removed orders
  }
  CHECK(checked >= 60);
}

// ---- Donchian: the stop is re-armed after an exit order expires unfilled ----
TEST_CASE("donchian_breakout: if the Day exit expires on a hole day, the next new bar re-arms the protective stop") {
  // Y trades like the standard Donchian scenario through its exit signal (date 107, close 9 < prior 2-bar low 12), but has
  // NO bar on date 108 (A defines the calendar). At the signal the stop (id 2) is cancelled and a Day exit (id 3) is sent;
  // on 108 Y cannot trade so the exit EXPIRES. On Y's next bar (109) the position is still open and unprotected, so the
  // remembered stop level (8.00) is re-placed (id 4). Close 9.5 is above the exit channel (min low 9) so it does not exit.
  std::vector<Bar> y = {mk(100, 10, 11, 9, 10),  mk(101, 10, 12, 10, 11), mk(102, 11, 12, 10, 11), mk(103, 11, 12, 10, 12),
                        mk(104, 11, 14, 11, 13), mk(105, 13, 15, 12, 14), mk(106, 14, 15, 13, 14), mk(107, 14, 14, 9, 9),
                        mk(109, 9.5, 10, 9, 9.5)};
  std::vector<Bar> a;
  for (int d = 100; d <= 109; ++d) a.push_back(mk(d, 50, 50, 50, 50));
  const AlignedData data = align_calendar({{"A", a}, {"Y", y}});
  auto s = make_strategy("donchian_breakout",
                         nlohmann::json::parse(R"({"symbol": "Y", "entry_n": 3, "exit_n": 2, "atr_period": 2, "atr_mult": 2, "allocation": 0.5})"));
  const EngineResult r = Engine(data, engine_cfg(1'000'000, free_costs())).run(*s);
  REQUIRE(r.fills.size() == 1);  // only the entry; the exit never filled
  REQUIRE(r.events.size() == 2);
  CHECK(r.events[0].kind == OrderEventKind::Cancelled);
  CHECK(r.events[0].id == 2);
  CHECK(r.events[0].date == 107);
  CHECK(r.events[1].kind == OrderEventKind::Expired);
  CHECK(r.events[1].id == 3);
  CHECK(r.events[1].date == 108);
  REQUIRE(r.open_positions.size() == 1);
  CHECK(r.open_positions[0].qty == 384);
  CHECK(r.orders_pending_at_end == 1);  // the re-armed stop (id 4)
}

// ---- the same harness with a WARM-UP window: history before eval_start feeds the indicators; nothing before it counts ----
TEST_CASE("look-ahead: every built-in is still identical through day t when it runs with a warm-up window and everything after t is garbage") {
  std::size_t checked = 0;
  for (const Builtin& bi : kBuiltins) {
    CAPTURE(bi.name);
    std::size_t traded = 0;
    for (std::uint64_t seed = 1; seed <= 2; ++seed) {
      const std::vector<SymbolSeries> base = base_series(seed);
      const AlignedData a = align_calendar(base);
      const std::size_t offset = 120;                       // the first 120 calendar days are warm-up
      const Date start = a.date(offset);
      const Run ra = run_builtin(bi, a, start);
      REQUIRE(ra.res.warmup_days == offset);
      REQUIRE(ra.res.equity_curve.size() == a.num_days() - offset);
      traded += ra.res.fills.size();
      for (std::size_t t : {130u, 149u, 150u, 158u, 250u, 330u}) {
        CAPTURE(seed);
        CAPTURE(t);
        const AlignedData b = with_garbage_after(base, a.date(t), seed);
        const Run rb = run_builtin(bi, b, start);
        CHECK(rb.res.warmup_days == offset);
        CHECK(prefix_equal(ra, rb, t, offset));
        ++checked;
      }
    }
    CHECK(traded > 0);
  }
  CHECK(checked == 60);
}
