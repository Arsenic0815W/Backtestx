// Hand-computed strategy scenarios. Bars are usually "flat" (open = high = low = close) so a fill price is simply
// that day's close value and every expected number can be done on paper. Costs are zero unless stated.
#include <stdexcept>

#include "bt/json_util.hpp"
#include "bt/strategies.hpp"
#include "bt/synthetic.hpp"
#include "doctest.h"
#include "test_util.hpp"

using namespace tu;
using nlohmann::json;

namespace {

std::vector<Bar> flat_series(const std::vector<double>& closes, Date d0 = 100, std::int64_t vol = 1'000'000) {
  std::vector<Bar> v;
  for (std::size_t i = 0; i < closes.size(); ++i) v.push_back(mk(d0 + static_cast<Date>(i), closes[i], closes[i], closes[i], closes[i], vol));
  return v;
}

EngineResult run_named(const AlignedData& d, const char* name, const char* params, const EngineConfig& c) {
  auto s = make_strategy(name, json::parse(params));
  return Engine(d, c).run(*s);
}

}  // namespace

// ============================================================ parameters & factory
TEST_CASE("factory: defaults are valid for every strategy, params may be null or {}") {
  for (const std::string& n : strategy_names()) {
    CAPTURE(n);
    CHECK_NOTHROW(make_strategy(n, json::object()));
    CHECK_NOTHROW(make_strategy(n, json()));
  }
  CHECK(strategy_names().size() == 5);
}

TEST_CASE("factory: strict parameter parsing") {
  CHECK_THROWS_AS(make_strategy("nope", json::object()), ConfigError);
  CHECK_THROWS_AS(make_strategy("sma_crossover", json::parse(R"({"fast_period": 5})")), ConfigError);   // unknown key
  CHECK_THROWS_AS(make_strategy("sma_crossover", json::parse(R"({"fast": "5"})")), ConfigError);        // wrong type
  CHECK_THROWS_AS(make_strategy("sma_crossover", json::parse(R"({"fast": 5.0})")), ConfigError);        // not an integer
  CHECK_THROWS_AS(make_strategy("sma_crossover", json::parse(R"({"fast": 0})")), ConfigError);          // below min
  CHECK_THROWS_AS(make_strategy("sma_crossover", json::parse(R"({"fast": 50, "slow": 50})")), ConfigError);
  CHECK_THROWS_AS(make_strategy("sma_crossover", json::parse(R"({"fast": 60, "slow": 50})")), ConfigError);
  CHECK_THROWS_AS(make_strategy("sma_crossover", json::parse(R"({"allocation": 0})")), ConfigError);
  CHECK_THROWS_AS(make_strategy("sma_crossover", json::parse(R"({"allocation": 1.5})")), ConfigError);
  CHECK_NOTHROW(make_strategy("sma_crossover", json::parse(R"({"allocation": 1})")));
  CHECK_THROWS_AS(make_strategy("sma_crossover", json::parse(R"({"symbol": 3})")), ConfigError);
  CHECK_THROWS_AS(make_strategy("sma_crossover", json::parse("[1]")), ConfigError);
  CHECK_THROWS_AS(make_strategy("rsi_mean_reversion", json::parse(R"({"buy_below": 70, "sell_above": 30})")), ConfigError);
  CHECK_THROWS_AS(make_strategy("rsi_mean_reversion", json::parse(R"({"buy_below": 50, "sell_above": 50})")), ConfigError);
  CHECK_THROWS_AS(make_strategy("rsi_mean_reversion", json::parse(R"({"period": 0})")), ConfigError);
  CHECK_THROWS_AS(make_strategy("rsi_mean_reversion", json::parse(R"({"buy_below": 101})")), ConfigError);
  CHECK_THROWS_AS(make_strategy("donchian_breakout", json::parse(R"({"atr_mult": 0})")), ConfigError);
  CHECK_THROWS_AS(make_strategy("donchian_breakout", json::parse(R"({"entry_n": 0})")), ConfigError);
  CHECK_THROWS_AS(make_strategy("momentum_rotation", json::parse(R"({"top_k": 0})")), ConfigError);
  CHECK_THROWS_AS(make_strategy("momentum_rotation", json::parse(R"({"universe": "A"})")), ConfigError);
  CHECK_THROWS_AS(make_strategy("momentum_rotation", json::parse(R"({"universe": ["A", ""]})")), ConfigError);
  CHECK_THROWS_AS(make_strategy("momentum_rotation", json::parse(R"({"rebalance_if_unchanged": 1})")), ConfigError);
  CHECK_THROWS_AS(make_strategy("buy_and_hold", json::parse(R"({"fast": 1})")), ConfigError);
  try {
    make_strategy("sma_crossover", json::parse(R"({"fast": "x"})"));
    FAIL("should have thrown");
  } catch (const ConfigError& e) {
    CHECK(std::string(e.what()).find("params.fast") != std::string::npos);  // names the offending key
  }
}

TEST_CASE("factory: data-dependent errors surface at run start, not mid-run; instances are fresh") {
  const AlignedData d = one_symbol(flat_series({10, 10, 10, 12}), "X");
  EngineConfig c = engine_cfg(1'000'000, free_costs());
  CHECK_THROWS_AS(run_named(d, "buy_and_hold", R"({"symbol": "NOPE"})", c), ConfigError);
  CHECK_THROWS_AS(run_named(d, "momentum_rotation", R"({"universe": ["X", "Y"]})", c), ConfigError);
  CHECK_THROWS_AS(run_named(d, "momentum_rotation", R"({"universe": ["X", "X"]})", c), ConfigError);
  CHECK_THROWS_AS(run_named(d, "momentum_rotation", R"({"universe": ["X"], "top_k": 2})", c), ConfigError);
  // Two make_strategy calls give two independent objects, and two runs with them agree exactly.
  auto s1 = make_strategy("sma_crossover", json::parse(R"({"fast": 2, "slow": 3})"));
  auto s2 = make_strategy("sma_crossover", json::parse(R"({"fast": 2, "slow": 3})"));
  CHECK(s1.get() != s2.get());
  const EngineResult a = Engine(d, c).run(*s1), b = Engine(d, c).run(*s2);
  CHECK(a.fills.size() == b.fills.size());
  CHECK(a.final_equity == b.final_equity);
}

// ============================================================ BuyAndHold
TEST_CASE("buy_and_hold: buys once on the first bar and never sells (hand-computed size)") {
  // Equity Rs 1,00,000, default allocation 0.99 -> 99,000 / close 100 = 990 shares; fills at the NEXT open (100).
  const AlignedData d = one_symbol(flat_series({100, 100, 100, 100, 100}));
  const EngineResult r = run_named(d, "buy_and_hold", "{}", engine_cfg(10'000'000, free_costs()));
  REQUIRE(r.fills.size() == 1);
  CHECK(r.fills[0].qty == 990);
  CHECK(r.fills[0].day_index == 1);
  CHECK(r.fills[0].price == 10'000);
  REQUIRE(r.open_positions.size() == 1);
  CHECK(r.open_positions[0].qty == 990);
  CHECK(r.trades.empty());
}

TEST_CASE("buy_and_hold: a rejected entry (gap up beyond the cash) is retried and re-sized on the next bar") {
  // Signal on day 0 close 100 -> 990 shares; day 1 opens at 200: 990 * 200 = 1,98,000 > cash -> rejected.
  // Day 1 close 200 -> 99,000 / 200 = 495 shares; fills day 2 open 200 -> 99,000.
  const AlignedData d = one_symbol(flat_series({100, 200, 200, 200}));
  const EngineResult r = run_named(d, "buy_and_hold", "{}", engine_cfg(10'000'000, free_costs()));
  REQUIRE(r.events.size() == 1);
  CHECK(r.events[0].kind == OrderEventKind::Rejected);
  CHECK(r.events[0].reason == RejectReason::InsufficientCash);
  REQUIRE(r.fills.size() == 1);
  CHECK(r.fills[0].qty == 495);
  CHECK(r.fills[0].day_index == 2);
}

TEST_CASE("buy_and_hold: default allocation is 0.99 and an explicit allocation overrides it") {
  const AlignedData d = one_symbol(flat_series({100, 100, 100}));
  CHECK(run_named(d, "buy_and_hold", "{}", engine_cfg(10'000'000, free_costs())).fills[0].qty == 990);
  CHECK(run_named(d, "buy_and_hold", R"({"allocation": 0.5})", engine_cfg(10'000'000, free_costs())).fills[0].qty == 500);
  CHECK(run_named(d, "buy_and_hold", R"({"allocation": 1})", engine_cfg(10'000'000, free_costs())).fills[0].qty == 1000);
}

TEST_CASE("buy_and_hold: trades only its own symbol; an entry that expires on a hole day is retried at the next NEW bar") {
  // BENCH trades every day (defines the calendar, must be ignored). X has bars on dates 100, 102, 103 - no bar on 101.
  //  day 0 (X bar): buy 990 (Day) -> day 1 has no X bar -> the order EXPIRES (known weakness: one bar of delay).
  //  day 1: no new X bar -> strategy does nothing.   day 2 (X bar): still flat -> buy again -> fills day 3 open.
  const AlignedData d = align_calendar({{"BENCH", flat_series({50, 50, 50, 50})},
                                        {"X", {mk(100, 100, 100, 100, 100), mk(102, 100, 100, 100, 100), mk(103, 100, 100, 100, 100)}}});
  const EngineResult r = run_named(d, "buy_and_hold", R"({"symbol": "X"})", engine_cfg(10'000'000, free_costs()));
  REQUIRE(r.events.size() == 1);
  CHECK(r.events[0].kind == OrderEventKind::Expired);
  CHECK(r.events[0].date == 101);
  REQUIRE(r.fills.size() == 1);
  CHECK(r.symbols[r.fills[0].symbol] == "X");
  CHECK(r.fills[0].day_index == 3);
  CHECK(r.fills[0].qty == 990);
}

// ============================================================ SMA crossover
// closes: 10 10 10 12 14 11 8 7 9 13  (flat bars, so fill price = that day's close value)
//   SMA(3): -    -    10   10.667 12   12.333 11   8.667 8    9.667
//   SMA(2): -    10   10   11     13   12.5   9.5  7.5   8    11
//   fast>slow?   -    -    NO(=)  YES YES  YES  NO   NO   NO(=) YES
// i2: fast == slow (10 == 10) -> NOT long (strict ">").   i3: first YES -> buy signal -> fills at day 4's open (14).
// i6: first NO -> sell signal -> fills at day 7's open (7).   i8: equal again -> flat.   i9: YES -> buy signal on the
// LAST bar -> never fills.
TEST_CASE("sma_crossover: hand-computed trades, with an equality bar and a last-bar signal") {
  const AlignedData d = one_symbol(flat_series({10, 10, 10, 12, 14, 11, 8, 7, 9, 13}));
  // Rs 10,000, allocation 0.5 -> at i3: 5,000 / 12 = 416 shares.  Buy 416 @ 14, sell 416 @ 7.
  const EngineResult r = run_named(d, "sma_crossover", R"({"fast": 2, "slow": 3, "allocation": 0.5})", engine_cfg(1'000'000, free_costs()));
  REQUIRE(r.fills.size() == 2);
  CHECK(r.fills[0].side == Side::Buy);
  CHECK(r.fills[0].day_index == 4);  // NOT 3: the equality bar at i2 produced no entry
  CHECK(r.fills[0].qty == 416);
  CHECK(r.fills[0].price == 1400);
  CHECK(r.fills[1].side == Side::Sell);
  CHECK(r.fills[1].day_index == 7);
  CHECK(r.fills[1].qty == 416);
  CHECK(r.fills[1].price == 700);
  CHECK(r.events.empty());
  REQUIRE(r.trades.size() == 1);
  CHECK(r.trades[0].gross_pnl == (700 - 1400) * 416);  // -Rs 2,912
  CHECK(r.orders_pending_at_end == 1);                 // the buy signalled on the final bar
  CHECK(r.final_cash == 1'000'000 + (700 - 1400) * 416);
}

TEST_CASE("sma_crossover: nothing trades during warm-up") {
  const AlignedData d = one_symbol(flat_series({10, 11, 12, 13, 14}));  // rising, but slow=10 never becomes ready
  const EngineResult r = run_named(d, "sma_crossover", R"({"fast": 2, "slow": 10})", engine_cfg(1'000'000, free_costs()));
  CHECK(r.fills.empty());
  CHECK(r.orders_pending_at_end == 0);
}

TEST_CASE("sma_crossover: a symbol with a hole sees only its own bars (same trades as without the hole)") {
  // Y's own closes (hole at date 105 removed): 10 10 10 12 14 8 7 9 13.  Signals: buy at its 4th bar, sell at its 6th.
  // Neither order is pending across the hole, so the fills must equal those of Y running alone on consecutive dates.
  const std::vector<double> closes = {10, 10, 10, 12, 14, 11, 8, 7, 9, 13};
  std::vector<Bar> y = flat_series(closes);
  y.erase(y.begin() + 5);                                   // drop date 105 (close 11)
  std::vector<Bar> alone_bars = y;
  for (std::size_t i = 0; i < alone_bars.size(); ++i) alone_bars[i].date = static_cast<Date>(1000 + i);
  const AlignedData alone = one_symbol(alone_bars, "Y");
  const AlignedData with_hole = align_calendar({{"A", flat_series(closes, 100)}, {"Y", y}});  // A fills date 105

  const char* p = R"({"symbol": "Y", "fast": 2, "slow": 3, "allocation": 0.5})";
  const EngineResult r1 = run_named(alone, "sma_crossover", p, engine_cfg(1'000'000, free_costs()));
  const EngineResult r2 = run_named(with_hole, "sma_crossover", p, engine_cfg(1'000'000, free_costs()));
  REQUIRE(r1.fills.size() == 2);
  REQUIRE(r2.fills.size() == 2);
  for (std::size_t i = 0; i < 2; ++i) {
    CHECK(r1.fills[i].side == r2.fills[i].side);
    CHECK(r1.fills[i].qty == r2.fills[i].qty);
    CHECK(r1.fills[i].price == r2.fills[i].price);
  }
  CHECK(r2.symbols[r2.fills[0].symbol] == "Y");
  CHECK(r1.final_cash == r2.final_cash);
}

// ============================================================ RSI mean reversion
// closes 10 9 8 9 10 11 12 11 10 9, RSI(2) (Wilder; |change| = 1 so RSI = 100*avg_gain):
//   i:     2    3    4    5     6      7       8        9
//   RSI:   0    50   75   87.5  93.75  46.875  23.4375  11.71875
TEST_CASE("rsi_mean_reversion: hand-computed trades") {
  const AlignedData d = one_symbol(flat_series({10, 9, 8, 9, 10, 11, 12, 11, 10, 9}));
  // Rs 10,000, allocation 0.5.
  //  i2 RSI 0 < 30  -> buy 5,000/8 = 625, fills day 3 open 9
  //  i4 RSI 75 > 70 -> sell,              fills day 5 open 11   => cash 10,000 - 5,625 + 6,875 = 11,250
  //  i8 RSI 23.4 < 30 -> buy floor(11,250*0.5/10) = 562, fills day 9 open 9 (still open at the end)
  const EngineResult r = run_named(d, "rsi_mean_reversion", R"({"period": 2, "buy_below": 30, "sell_above": 70, "allocation": 0.5})",
                                   engine_cfg(1'000'000, free_costs()));
  REQUIRE(r.fills.size() == 3);
  CHECK(r.fills[0].day_index == 3);
  CHECK(r.fills[0].qty == 625);
  CHECK(r.fills[0].price == 900);
  CHECK(r.fills[1].day_index == 5);
  CHECK(r.fills[1].side == Side::Sell);
  CHECK(r.fills[1].price == 1100);
  CHECK(r.fills[2].day_index == 9);
  CHECK(r.fills[2].qty == 562);
  CHECK(r.fills[2].price == 900);
  REQUIRE(r.trades.size() == 1);
  CHECK(r.trades[0].gross_pnl == (1100 - 900) * 625);  // +Rs 1,250
  REQUIRE(r.open_positions.size() == 1);
  CHECK(r.open_positions[0].qty == 562);
}

TEST_CASE("rsi_mean_reversion: thresholds are strict - RSI exactly at the threshold does nothing") {
  const AlignedData d = one_symbol(flat_series({10, 9, 8, 9, 10, 11}));
  EngineConfig c = engine_cfg(1'000'000, free_costs());
  {  // sell_above = 50: RSI is exactly 50 at i3 -> NO sell there; it sells at i4 (RSI 75) -> fills day 5, not day 4
    const EngineResult r = run_named(d, "rsi_mean_reversion", R"({"period": 2, "buy_below": 30, "sell_above": 50, "allocation": 0.5})", c);
    REQUIRE(r.fills.size() == 2);
    CHECK(r.fills[1].side == Side::Sell);
    CHECK(r.fills[1].day_index == 5);
  }
  {  // RSI is EXACTLY 50 at every bar of 10 11 10 10 10 10 (gains and losses decay identically): with buy_below = 50
     // the strict "<" never buys. (A "<=" would buy at the first ready bar.)
    const AlignedData flat50 = one_symbol(flat_series({10, 11, 10, 10, 10, 10}));
    const EngineResult r = run_named(flat50, "rsi_mean_reversion", R"({"period": 2, "buy_below": 50, "sell_above": 80})", c);
    CHECK(r.fills.empty());
    CHECK(r.orders_pending_at_end == 0);
  }
  {  // buy_below = 0: RSI is exactly 0 at i2 and 0 < 0 is false -> never buys
    const EngineResult r = run_named(d, "rsi_mean_reversion", R"({"period": 2, "buy_below": 0, "sell_above": 70})", c);
    CHECK(r.fills.empty());
  }
}

// ============================================================ Donchian breakout
// entry_n=3, exit_n=2, atr_period=2, atr_mult=2.   Bars (o,h,l,c):
//   i0 10 11  9 10 | i1 10 12 10 11 | i2 11 12 10 11 | i3 11 12 10 12 | i4 11 14 11 13 | i5 13 15 12 14
//   i6 14 15 13 14 | i7 14 14  9  9 | i8  9 10 8.5 9
// i3: close 12 EQUALS the highest high of the previous 3 bars (12) -> no entry (strict).
// i4: previous 3 highs (i1..i3) = 12 -> close 13 > 12 -> ENTRY.  (If today's high 14 were included the channel would
//     be 14 and there would be no entry: this is the "prior bars only" check.)
// ATR(2): TR = 2,2,2,2,3 -> ATR i4 = (2*1+3)/2 = 2.5 -> stop = 13 - 2*2.5 = 8.00.
// i7: previous 2 lows (i5,i6) = 12,13 -> 12; close 9 < 12 -> EXIT.
namespace {
std::vector<Bar> donchian_bars() {
  return {mk(100, 10, 11, 9, 10),  mk(101, 10, 12, 10, 11), mk(102, 11, 12, 10, 11), mk(103, 11, 12, 10, 12),
          mk(104, 11, 14, 11, 13), mk(105, 13, 15, 12, 14), mk(106, 14, 15, 13, 14), mk(107, 14, 14, 9, 9),
          mk(108, 9, 10, 8.5, 9)};
}
const char* kDon = R"({"entry_n": 3, "exit_n": 2, "atr_period": 2, "atr_mult": 2, "allocation": 0.5})";
}  // namespace

TEST_CASE("donchian_breakout: entry on a strict breakout of the PRIOR channel, exit on the exit channel (hand-computed)") {
  // Rs 10,000, allocation 0.5 -> 5,000 / 13 = 384 shares.  Buy id1 + stop id2 at i4's close.
  const EngineResult r = run_named(AlignedData(one_symbol(donchian_bars())), "donchian_breakout", kDon, engine_cfg(1'000'000, free_costs()));
  REQUIRE(r.fills.size() == 2);
  CHECK(r.fills[0].day_index == 5);  // entry at i5's open 13 (NOT i4: i3's equality bar gave no entry)
  CHECK(r.fills[0].qty == 384);
  CHECK(r.fills[0].price == 1300);
  CHECK(r.fills[1].day_index == 8);  // exit signal at i7 close, fills i8 open 9
  CHECK(r.fills[1].price == 900);
  CHECK(r.fills[1].order_id == 3);
  // The protective stop (id 2) was cancelled when the exit was submitted.
  REQUIRE(r.events.size() == 1);
  CHECK(r.events[0].kind == OrderEventKind::Cancelled);
  CHECK(r.events[0].id == 2);
  CHECK(r.events[0].date == 107);
  REQUIRE(r.trades.size() == 1);
  CHECK(r.trades[0].gross_pnl == (900 - 1300) * 384);  // -Rs 1,536
  CHECK(r.orders_pending_at_end == 0);
}

TEST_CASE("donchian_breakout: exit needs a STRICT break of the exit channel (equality holds the position)") {
  std::vector<Bar> b = donchian_bars();
  b[7] = mk(107, 14, 14, 12, 12);  // close 12 == prior 2-bar low 12 -> no exit
  b[8] = mk(108, 12, 13, 11, 12.5);  // prior lows (i6,i7) = 13,12 -> 12; close 12.5 > 12 -> still no exit
  const EngineResult r = run_named(one_symbol(b), "donchian_breakout", kDon, engine_cfg(1'000'000, free_costs()));
  REQUIRE(r.fills.size() == 1);  // only the entry
  REQUIRE(r.open_positions.size() == 1);
  CHECK(r.open_positions[0].qty == 384);
  CHECK(r.events.empty());
  CHECK(r.orders_pending_at_end == 1);  // the protective stop is still working
}

TEST_CASE("donchian_breakout: a gap down through the protective stop fills at the open") {
  std::vector<Bar> b = donchian_bars();
  b[6] = mk(106, 7, 8, 6, 7);  // gaps below the stop at 8.00
  b[7] = mk(107, 7, 8, 6, 7);
  b[8] = mk(108, 7, 8, 6, 7);
  const EngineResult r = run_named(one_symbol(b), "donchian_breakout", kDon, engine_cfg(1'000'000, free_costs()));
  REQUIRE(r.fills.size() == 2);
  CHECK(r.fills[1].order_id == 2);       // the stop
  CHECK(r.fills[1].day_index == 6);
  CHECK(r.fills[1].price == 700);        // min(open 7, stop 8)
  CHECK(r.events.empty());               // nothing to cancel, nothing rejected
  REQUIRE(r.trades.size() == 1);
  CHECK(r.trades[0].gross_pnl == (700 - 1300) * 384);  // -Rs 2,304
  CHECK(r.orders_pending_at_end == 0);   // no leftover stop
}

TEST_CASE("donchian_breakout: a rejected entry does not leave a live stop; the strategy re-enters cleanly") {
  // i5 opens at 30: 384 * 30 = 11,520 > Rs 10,000 -> the entry (id1) is rejected. The stop (id2) is never touched, so it
  // stays pending as an orphan until the strategy cancels it at i5's close. At that close the breakout is still on (previous 3 highs = 14, close 30): re-enter with
  // 5,000/30 = 166 shares. ATR(i5): TR = max(2, |31-13|, |29-13|) = 18 -> (2.5*1 + 18)/2 = 10.25 -> stop = 30 - 20.5 = 9.50.
  std::vector<Bar> b = donchian_bars();
  b.resize(7);
  b[5] = mk(105, 30, 31, 29, 30);
  b[6] = mk(106, 30, 31, 29, 30);
  const EngineResult r = run_named(one_symbol(b), "donchian_breakout", kDon, engine_cfg(1'000'000, free_costs()));
  REQUIRE(r.events.size() == 2);
  CHECK(r.events[0].id == 1);
  CHECK(r.events[0].kind == OrderEventKind::Rejected);
  CHECK(r.events[0].reason == RejectReason::InsufficientCash);
  // The untriggered stop (id 2) was left pending on a flat position; the strategy's own reconcile step cancels it.
  CHECK(r.events[1].id == 2);
  CHECK(r.events[1].kind == OrderEventKind::Cancelled);
  CHECK(r.events[1].date == 105);
  REQUIRE(r.fills.size() == 1);
  CHECK(r.fills[0].order_id == 3);
  CHECK(r.fills[0].qty == 166);
  CHECK(r.fills[0].price == 3000);
  CHECK(r.orders_pending_at_end == 1);  // exactly one live stop (id 4)
}

TEST_CASE("donchian_breakout: after a PARTIAL entry fill the stop is resized to the position") {
  // Volume cap: each bar has volume 1000, cap 10 % -> only 100 shares per bar. The entry (384 shares, Day) fills 100
  // and the rest expires; the stop (384, GTC) no longer matches the 100-share position, so it is cancelled and replaced.
  std::vector<Bar> b = donchian_bars();
  for (Bar& x : b) x.volume = 1000;
  EngineConfig c = engine_cfg(1'000'000, free_costs());
  c.max_volume_participation = 0.1;
  const EngineResult r = run_named(one_symbol(b), "donchian_breakout", kDon, c);
  REQUIRE(r.fills.size() == 2);
  CHECK(r.fills[0].qty == 100);  // partial entry at i5
  CHECK(r.fills[1].qty == 100);  // exit sells what is held
  // Cancelled: the stale 384-share stop (id 2) at i5, then the replacement (id 3) at the exit on i7.
  int cancelled = 0;
  for (const auto& e : r.events)
    if (e.kind == OrderEventKind::Cancelled) { ++cancelled; CHECK((e.id == 2 || e.id == 3)); }
  CHECK(cancelled == 2);
  CHECK(r.open_positions.empty());
}

// ============================================================ Momentum rotation
namespace {
// Dates: Jan 29, 30, 31, Feb 1, 2, 5, 6, 7, Mar 1, Mar 4 (10 bars; Feb 1 and Mar 1 are the first bars of their months).
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
AlignedData momentum_data(double c_march = 110) {
  const auto d = momentum_dates();
  return align_calendar({{"A", on_dates({100, 101, 102, 103, 104, 105, 106, 107, 90, 91}, d)},
                         {"B", on_dates({100, 100, 100, 110, 111, 112, 113, 114, 120, 125}, d)},
                         {"C", on_dates({100, 99, 98, 97, 96, 95, 94, 93, c_march, 111}, d)}});
}
}  // namespace

// lookback 3 bars, top_k 1, allocation 0.95, Rs 1,00,000.
//  Jan 29-31: fewer than 4 bars -> nothing rankable -> the month is NOT consumed.
//  Feb 1 (i3): returns A 103/100-1 = 3 %, B 110/100-1 = 10 %, C 97/100-1 = -3 % -> hold B.
//     investable 95,000 -> floor(95,000/110) = 863 shares; fills i4 open 111.
//  Mar 1 (i8): A 90/105-1 = -14.3 %, B 120/112-1 = 7.1 %, C 110/95-1 = 15.8 % -> hold C.
//     equity = cash 4,207 + 863*120 = 107,767 -> investable 102,378.65 -> floor(/110) = 930 C shares.
//     orders: SELL B 863 (id 2) then BUY C 930 (id 3); both fill i9: B @125 = 107,875 funds C @111 = 1,03,230.
TEST_CASE("momentum_rotation: monthly rotation, hand-computed to the share") {
  const EngineResult r = run_named(momentum_data(), "momentum_rotation", R"({"lookback": 3, "top_k": 1})", engine_cfg(10'000'000, free_costs()));
  REQUIRE(r.fills.size() == 3);
  CHECK(r.symbols[r.fills[0].symbol] == "B");
  CHECK(r.fills[0].day_index == 4);
  CHECK(r.fills[0].qty == 863);
  CHECK(r.fills[0].price == 11100);
  CHECK(r.symbols[r.fills[1].symbol] == "B");
  CHECK(r.fills[1].side == Side::Sell);
  CHECK(r.fills[1].day_index == 9);
  CHECK(r.fills[1].qty == 863);
  CHECK(r.fills[1].price == 12500);
  CHECK(r.symbols[r.fills[2].symbol] == "C");
  CHECK(r.fills[2].qty == 930);
  CHECK(r.fills[2].price == 11100);
  CHECK(r.events.empty());
  // cash: 4,207 + 107,875 - 1,03,230 = 8,852 rupees
  CHECK(r.final_cash == 885'200);
  REQUIRE(r.open_positions.size() == 1);
  CHECK(r.symbols[r.open_positions[0].symbol] == "C");
}

TEST_CASE("momentum_rotation: a month is only consumed once something was rankable") {
  // lookback 2: the first rankable bar is Jan 31 (3 bars of history), in the SAME month as the two unrankable bars
  // before it. Jan 29/30 must not use up January. Jan 31: A 102/100-1 = 2 %, B 0 %, C -2 % -> buy A:
  // floor(95,000 / 102) = 931 shares, filling on Feb 1 (day 3) at A's open 103.
  const EngineResult r = run_named(momentum_data(), "momentum_rotation", R"({"lookback": 2, "top_k": 1})", engine_cfg(10'000'000, free_costs()));
  REQUIRE(r.fills.size() >= 1);
  CHECK(r.symbols[r.fills[0].symbol] == "A");
  CHECK(r.fills[0].day_index == 3);
  CHECK(r.fills[0].qty == 931);
  CHECK(r.fills[0].price == 10300);
}

TEST_CASE("momentum_rotation: top_k = 2 splits equally; buys are submitted in ascending symbol order") {
  // Feb 1: top 2 are B (10 %) and A (3 %). Weights 1/2: 47,500 each. A: floor(47,500/103) = 461, B: floor(47,500/110) = 431.
  const auto d = momentum_dates();
  const AlignedData six = align_calendar({{"A", on_dates({100, 101, 102, 103, 104, 105}, std::vector<Date>(d.begin(), d.begin() + 6))},
                                          {"B", on_dates({100, 100, 100, 110, 111, 112}, std::vector<Date>(d.begin(), d.begin() + 6))},
                                          {"C", on_dates({100, 99, 98, 97, 96, 95}, std::vector<Date>(d.begin(), d.begin() + 6))}});
  const EngineResult r = run_named(six, "momentum_rotation", R"({"lookback": 3, "top_k": 2})", engine_cfg(10'000'000, free_costs()));
  REQUIRE(r.fills.size() == 2);
  CHECK(r.symbols[r.fills[0].symbol] == "A");
  CHECK(r.fills[0].qty == 461);
  CHECK(r.symbols[r.fills[1].symbol] == "B");
  CHECK(r.fills[1].qty == 431);
  CHECK(r.fills[0].order_id < r.fills[1].order_id);
}

TEST_CASE("momentum_rotation: unchanged selection trades nothing (no churn) unless rebalance_if_unchanged") {
  // C's March close is 100 (below B's return) so B is still the top name on Mar 1 and is already held.
  {
    const EngineResult r = run_named(momentum_data(100), "momentum_rotation", R"({"lookback": 3, "top_k": 1})", engine_cfg(10'000'000, free_costs()));
    REQUIRE(r.fills.size() == 1);  // only the February purchase
    CHECK(r.events.empty());
  }
  {  // forced rebalance: investable 102,378.65 / 120 = 853 target vs 863 held -> sell 10 at the Mar 4 open (125)
    const EngineResult r = run_named(momentum_data(100), "momentum_rotation",
                                     R"({"lookback": 3, "top_k": 1, "rebalance_if_unchanged": true})", engine_cfg(10'000'000, free_costs()));
    REQUIRE(r.fills.size() == 2);
    CHECK(r.fills[1].side == Side::Sell);
    CHECK(r.fills[1].qty == 10);
    CHECK(r.fills[1].price == 12500);
  }
}

TEST_CASE("momentum_rotation: a held symbol with a one-day hole on rebalance day is NOT sold") {
  // B lacks Mar 1 but has a bar on Feb 29 (1 day old, within max_stale_days = 7), so it is still ranked from its own bars.
  const std::vector<const char*> ds = {"2024-01-29", "2024-01-30", "2024-01-31", "2024-02-01", "2024-02-02", "2024-02-05",
                                       "2024-02-06", "2024-02-07", "2024-02-29", "2024-03-01", "2024-03-04"};
  std::vector<Date> d;
  for (const char* s : ds) d.push_back(parse_date(s).value());
  std::vector<Bar> b = on_dates({100, 100, 100, 110, 111, 112, 113, 114, 115, 0, 117}, d);
  b.erase(b.begin() + 9);  // no bar on Mar 1
  const AlignedData data = align_calendar({{"A", on_dates({100, 101, 102, 103, 103, 103, 103, 103, 103, 103, 103}, d)}, {"B", b}});
  // Mar 1: B has no bar today; its own bars give 115 (Feb 29) vs 112 three bars earlier = +2.7 %, A is 103/103-1 = 0 %,
  // so B stays the top name and is already held -> no trade.
  const EngineResult r = run_named(data, "momentum_rotation", R"({"lookback": 3, "top_k": 1})", engine_cfg(10'000'000, free_costs()));
  for (const Fill& f : r.fills) CHECK(f.side == Side::Buy);  // never sold
  CHECK(r.fills.size() == 1);
  CHECK(r.events.empty());
}

TEST_CASE("momentum_rotation: a symbol that stopped trading is not ranked on stale data - and its holding is stuck") {
  // Y trades Jan 29 - Feb 7 (closes 100 100 100 200 210 220 230 240), then never again. A rises slowly, Z is flat.
  //  Feb 1: Y has the best 3-bar return (+100 %) -> buy floor(95,000/200) = 475 (fills Feb 2 at 210 = Rs 99,750).
  //  Mar 1: Y's last bar is 23 days old (> 7) -> NOT ranked; A (+2.9 %) is selected instead. So: sell Y, buy A.
  //  But Y has no bar, so the Day sell cannot fill and expires; with only Rs 250 of cash the buy of A is REJECTED.
  //  (A real "delisted holding" cannot be sold in this engine; the test pins that limitation down.)
  const auto d = momentum_dates();
  const AlignedData data = align_calendar(
      {{"A", on_dates({100, 101, 102, 103, 104, 105, 106, 107, 108, 109}, d)},
       {"Y", on_dates({100, 100, 100, 200, 210, 220, 230, 240}, std::vector<Date>(d.begin(), d.begin() + 8))},
       {"Z", on_dates({100, 100, 100, 100, 100, 100, 100, 100, 100, 100}, d)}});
  const EngineResult r = run_named(data, "momentum_rotation", R"({"lookback": 3, "top_k": 1})", engine_cfg(10'000'000, free_costs()));
  REQUIRE(r.fills.size() == 1);
  CHECK(r.symbols[r.fills[0].symbol] == "Y");
  CHECK(r.fills[0].side == Side::Buy);
  CHECK(r.fills[0].qty == 475);
  CHECK(r.fills[0].price == 21000);
  REQUIRE(r.events.size() == 2);
  CHECK(r.events[0].kind == OrderEventKind::Rejected);   // buy A (id 3): not enough cash
  CHECK(r.events[0].reason == RejectReason::InsufficientCash);
  CHECK(r.events[1].kind == OrderEventKind::Expired);    // sell Y (id 2): no bar to trade on
  CHECK(r.events[1].id == 2);
}

// ============================================================ behaviour on synthetic data
TEST_CASE("momentum_rotation on synthetic data: the 5 % allocation buffer avoids cash rejections (measured on these seeds)") {
  std::size_t cash_rejects = 0, fills = 0;
  for (std::uint64_t seed = 1; seed <= 5; ++seed) {
    SynthParams p;
    p.start = parse_date("2022-01-03").value();
    p.num_bars = 500;
    std::vector<SymbolSeries> series;
    for (int k = 0; k < 4; ++k) {
      p.seed = seed * 10 + static_cast<std::uint64_t>(k);
      p.start_price = 100.0 * (k + 1);
      p.annual_vol = 0.3;
      series.push_back({std::string(1, static_cast<char>('A' + k)), generate_gbm(p)});
    }
    const AlignedData d = align_calendar(std::move(series));
    EngineConfig c;
    c.initial_cash = to_paise(1'000'000.0);
    const EngineResult r = run_named(d, "momentum_rotation", R"({"lookback": 40, "top_k": 2})", c);
    fills += r.fills.size();
    for (const auto& e : r.events)
      if (e.kind == OrderEventKind::Rejected && e.reason == RejectReason::InsufficientCash) ++cash_rejects;
  }
  CHECK(fills > 10);           // it actually traded
  CHECK(cash_rejects == 0);    // measured: zero on these 5 seeds (not a general guarantee)
}
