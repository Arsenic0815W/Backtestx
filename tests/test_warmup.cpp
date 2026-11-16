// Warm-up / evaluation-window tests. Rule under test: nothing counts before eval_start. Days before it call on_bar (so
// indicators warm up) but all orders are discarded, nothing is recorded, ids restart, and the equity curve starts at
// eval_start with initial cash. The first decision that counts is made at the CLOSE of the first evaluation day.
#include "bt/strategies.hpp"
#include "doctest.h"
#include "test_util.hpp"

using namespace tu;
using nlohmann::json;

namespace {

std::vector<Bar> flat_series(const std::vector<double>& closes, Date d0 = 100) {
  std::vector<Bar> v;
  for (std::size_t i = 0; i < closes.size(); ++i)
    v.push_back(mk(d0 + static_cast<Date>(i), closes[i], closes[i], closes[i], closes[i]));
  return v;
}

EngineConfig with_start(Paise cash, Date start) {
  EngineConfig c = engine_cfg(cash, free_costs());
  c.eval_start = start;
  return c;
}

EngineResult run_named(const AlignedData& d, const char* name, const char* params, const EngineConfig& c) {
  auto s = make_strategy(name, json::parse(params));
  return Engine(d, c).run(*s);
}

std::vector<Bar> donchian_bars() {
  return {mk(100, 10, 11, 9, 10),  mk(101, 10, 12, 10, 11), mk(102, 11, 12, 10, 11), mk(103, 11, 12, 10, 12),
          mk(104, 11, 14, 11, 13), mk(105, 13, 15, 12, 14), mk(106, 14, 15, 13, 14), mk(107, 14, 14, 9, 9),
          mk(108, 9, 10, 8.5, 9)};
}
const char* kDon = R"({"entry_n": 3, "exit_n": 2, "atr_period": 2, "atr_mult": 2, "allocation": 0.5})";

std::vector<Date> momentum_dates() {
  std::vector<Date> d;
  for (const char* s : {"2024-01-29", "2024-01-30", "2024-01-31", "2024-02-01", "2024-02-02", "2024-02-05", "2024-02-06",
                        "2024-02-07", "2024-03-01", "2024-03-04"})
    d.push_back(parse_date(s).value());
  return d;
}
std::vector<Bar> on_dates(const std::vector<double>& closes, const std::vector<Date>& dates) {
  std::vector<Bar> v;
  for (std::size_t i = 0; i < closes.size(); ++i) v.push_back(mk(dates[i], closes[i], closes[i], closes[i], closes[i]));
  return v;
}
AlignedData momentum_data() {
  const auto d = momentum_dates();
  return align_calendar({{"A", on_dates({100, 101, 102, 103, 104, 105, 106, 107, 90, 91}, d)},
                         {"B", on_dates({100, 100, 100, 110, 111, 112, 113, 114, 120, 125}, d)},
                         {"C", on_dates({100, 99, 98, 97, 96, 95, 94, 93, 110, 111}, d)}});
}

}  // namespace

TEST_CASE("warm-up: nothing counts before the start date") {
  // 10 flat bars, dates 100..109, eval_start 105 -> warm-up days are idx 0..4, evaluation days idx 5..9.
  const AlignedData d = one_symbol(flat_bars(10));
  std::vector<bool> warm_flags;
  Scripted s([&](Context& c) {
    warm_flags.push_back(c.in_warmup());
    c.buy(0, 1);                                              // one market buy every day
    const OrderId lim = c.limit(0, Side::Buy, 1, 500.0, TimeInForce::GTC);  // plus a resting order that is never touched ...
    if (c.in_warmup()) c.cancel(lim);                         // ... and, during warm-up only, cancelled again
  });
  const EngineResult r = Engine(d, with_start(100'000'000, 105)).run(s);

  REQUIRE(warm_flags.size() == 10);
  for (std::size_t i = 0; i < 10; ++i) CHECK(warm_flags[i] == (i < 5));
  CHECK(r.warmup_days == 5);
  CHECK(r.warmup_orders_discarded == 5);          // the five market buys (the limits were cancelled before day end)
  // The equity curve starts at the start date with exactly the initial cash.
  REQUIRE(r.equity_curve.size() == 5);
  CHECK(r.equity_curve.front().date == 105);
  CHECK(r.equity_curve.front().equity == 100'000'000);
  CHECK(r.equity_curve.front().positions_value == 0);
  // Nothing dated before the start: warm-up cancellations are not recorded, and order ids restart at 1.
  for (const auto& e : r.events) CHECK(e.date >= 105);
  CHECK(r.events.empty());
  REQUIRE(r.fills.size() == 4);                   // buys submitted on days 105..108 fill on 106..109
  for (const Fill& f : r.fills) CHECK(f.date > 105);
  CHECK(r.fills.front().order_id == 1);           // first order of the evaluation window
  CHECK(r.fills.front().day_index == 6);
  CHECK(r.fills.front().position_after == 1);
}

TEST_CASE("warm-up: the boundary - a date before the start is warm-up, the start date itself is evaluated") {
  const AlignedData d = one_symbol(flat_bars(6));  // dates 100..105
  Scripted s([](Context&) {});
  // start exactly on a bar date
  CHECK(Engine(d, with_start(1'000'000, 103)).run(s).equity_curve.front().date == 103);
  // start between bar dates: the first date AFTER it
  const AlignedData gap = one_symbol({mk(100, 1, 1, 1, 1), mk(101, 1, 1, 1, 1), mk(104, 1, 1, 1, 1), mk(105, 1, 1, 1, 1)});
  const EngineResult r = Engine(gap, with_start(1'000'000, 102)).run(s);
  REQUIRE(r.equity_curve.size() == 2);
  CHECK(r.equity_curve.front().date == 104);
  CHECK(r.warmup_days == 2);
}

TEST_CASE("warm-up: no eval_start, or one before the first bar, changes nothing") {
  const AlignedData d = one_symbol(flat_bars(8));
  auto run = [&](std::optional<Date> start) {
    Scripted s([](Context& c) { if (c.day_index() == 1) c.buy(0, 3); });
    EngineConfig c = engine_cfg(10'000'000, free_costs());
    c.eval_start = start;
    return Engine(d, c).run(s);
  };
  const EngineResult a = run(std::nullopt), b = run(Date{50});
  CHECK(b.warmup_days == 0);
  CHECK(a.final_equity == b.final_equity);
  CHECK(a.fills.size() == b.fills.size());
  CHECK(a.equity_curve.size() == b.equity_curve.size());
  CHECK(a.equity_curve.size() == 8);
}

TEST_CASE("warm-up: a start after all the data leaves an empty window, not a crash") {
  const AlignedData d = one_symbol(flat_bars(5));
  auto s = make_strategy("buy_and_hold", json::object());
  const EngineResult r = Engine(d, with_start(1'000'000, 500)).run(*s);
  CHECK(r.equity_curve.empty());
  CHECK(r.fills.empty());
  CHECK(r.warmup_days == 5);
  CHECK(r.final_equity == 1'000'000);
}

// SMA(2,3), closes 10 10 10 12 14 11 8 7 9 13 (flat bars, fill price = that day's close value), eval_start = idx 4.
//   With warm-up, SMA(2)=13 > SMA(3)=12 is ALREADY true on the first evaluation day (idx 4) -> buy at that close:
//   floor(5,000 / 14) = 357 shares, filling at idx 5's open (11).
//   Without warm-up (data starting at idx 4: 14 11 8 7 9 13) the SMAs only compare on the last bar - no trade at all.
TEST_CASE("warm-up: indicators use the history before the start, so a strategy can act on day 1") {
  const std::vector<double> closes = {10, 10, 10, 12, 14, 11, 8, 7, 9, 13};
  const AlignedData d = one_symbol(flat_series(closes));
  const char* p = R"({"fast": 2, "slow": 3, "allocation": 0.5})";
  const EngineResult with = run_named(d, "sma_crossover", p, with_start(1'000'000, 104));
  REQUIRE(with.fills.size() >= 1);
  CHECK(with.equity_curve.front().date == 104);
  CHECK(with.fills[0].day_index == 5);
  CHECK(with.fills[0].qty == 357);
  CHECK(with.fills[0].price == 1100);

  const AlignedData sliced = one_symbol(flat_series({14, 11, 8, 7, 9, 13}, 104));
  const EngineResult without = run_named(sliced, "sma_crossover", p, engine_cfg(1'000'000, free_costs()));
  CHECK(without.fills.empty());
}

// ---- day-1 readiness for EVERY built-in strategy: when the entry condition already holds on the first evaluation day,
//      an order goes out that day (so it fills on the next bar). Warm-up must not leave state that blocks this.
TEST_CASE("warm-up day-1 readiness: buy_and_hold") {
  const AlignedData d = one_symbol(flat_series({100, 100, 100, 100, 100, 100}));
  const EngineResult r = run_named(d, "buy_and_hold", "{}", with_start(10'000'000, 103));
  REQUIRE(r.fills.size() == 1);
  CHECK(r.fills[0].day_index == 4);  // decided at the close of day idx 3 (the first evaluation day)
  CHECK(r.fills[0].qty == 990);
}

TEST_CASE("warm-up day-1 readiness: sma_crossover") {
  const AlignedData d = one_symbol(flat_series({10, 11, 12, 13, 14, 15, 16, 17}));  // rising: fast > slow from idx 2 on
  const EngineResult r = run_named(d, "sma_crossover", R"({"fast": 2, "slow": 3, "allocation": 0.5})", with_start(1'000'000, 105));
  REQUIRE(r.fills.size() >= 1);
  CHECK(r.fills[0].day_index == 6);  // first evaluation day idx 5 -> fill idx 6
}

TEST_CASE("warm-up day-1 readiness: rsi_mean_reversion") {
  const AlignedData d = one_symbol(flat_series({10, 9, 8, 7, 6, 5, 4, 3}));  // RSI(2) is 0 from idx 2 on: oversold already
  const EngineResult r = run_named(d, "rsi_mean_reversion", R"({"period": 2, "buy_below": 30, "sell_above": 70, "allocation": 0.5})",
                                   with_start(1'000'000, 104));
  REQUIRE(r.fills.size() >= 1);
  CHECK(r.fills[0].day_index == 5);
}

TEST_CASE("warm-up day-1 readiness: donchian_breakout (breakout happens on the first evaluation day)") {
  const AlignedData d = one_symbol(donchian_bars());
  const EngineResult r = run_named(d, "donchian_breakout", kDon, with_start(1'000'000, 104));
  REQUIRE(r.fills.size() >= 1);
  CHECK(r.fills[0].day_index == 5);
  CHECK(r.fills[0].qty == 384);
  CHECK(r.fills[0].price == 1300);
  CHECK(r.fills[0].order_id == 1);   // ids restarted: the entry is the first order of the window
}

TEST_CASE("warm-up state desync: a Donchian entry made (and discarded) in warm-up leaves no stale stop or state") {
  // The breakout happens at idx 4, which is warm-up here (eval_start = idx 5): the strategy submits a buy and a stop,
  // both discarded, and its internal stop level stays set. It must not place a stray stop later.
  const AlignedData d = one_symbol(donchian_bars());
  const EngineResult r = run_named(d, "donchian_breakout", kDon, with_start(1'000'000, 105));
  CHECK(r.warmup_orders_discarded >= 2);
  CHECK(r.fills.empty());                  // no breakout on any evaluation day (close 14 vs channel 15 on idx 5, ...)
  CHECK(r.orders_pending_at_end == 0);     // and no leftover working order
  CHECK(r.events.empty());
}

TEST_CASE("warm-up day-1 readiness: momentum_rotation rebalances on the first evaluation day even mid-month") {
  // eval_start = Feb 5 (idx 5). Feb 1 was a warm-up day: acting then would have used up February and left the portfolio
  // in cash until March. Instead the rebalance happens on Feb 5: B leads (112/100 - 1 = 12 %), 95,000/112 = 848 shares,
  // filling on Feb 6 (idx 6) at B's open 113.
  const EngineResult r = run_named(momentum_data(), "momentum_rotation", R"({"lookback": 3, "top_k": 1})",
                                   with_start(10'000'000, parse_date("2024-02-05").value()));
  REQUIRE(r.fills.size() >= 1);
  CHECK(r.symbols[r.fills[0].symbol] == "B");
  CHECK(r.fills[0].day_index == 6);
  CHECK(r.fills[0].qty == 848);
  CHECK(r.fills[0].price == 11300);
  CHECK(r.equity_curve.front().date == parse_date("2024-02-05").value());
}

TEST_CASE("warm-up: every strategy run with the same start gets the same window and the same starting equity") {
  const AlignedData d = momentum_data();
  const Date start = parse_date("2024-02-01").value();
  const std::vector<std::pair<const char*, const char*>> runs = {
      {"buy_and_hold", R"({"symbol": "B"})"},
      {"sma_crossover", R"({"symbol": "A", "fast": 2, "slow": 3})"},
      {"rsi_mean_reversion", R"({"symbol": "C", "period": 2})"},
      {"donchian_breakout", R"({"symbol": "B", "entry_n": 2, "exit_n": 2, "atr_period": 2})"},
      {"momentum_rotation", R"({"lookback": 3, "top_k": 2})"}};
  std::vector<Date> dates;
  for (const auto& run : runs) {
    const char* name = run.first;
    const char* params = run.second;
    CAPTURE(name);
    const EngineResult r = run_named(d, name, params, with_start(10'000'000, start));
    REQUIRE_FALSE(r.equity_curve.empty());
    CHECK(r.equity_curve.front().date == start);
    CHECK(r.equity_curve.front().equity == 10'000'000);
    CHECK(r.equity_curve.size() == 7);   // Feb 1 .. Mar 4
    CHECK(r.warmup_days == 3);
    for (const Fill& f : r.fills) CHECK(f.date > start);
    std::vector<Date> these;
    for (const auto& rec : r.equity_curve) these.push_back(rec.date);
    if (dates.empty()) dates = these;
    CHECK(these == dates);               // identical curve dates for every strategy
  }
}
