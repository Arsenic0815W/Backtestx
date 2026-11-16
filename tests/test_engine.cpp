#include <cmath>
#include <random>
#include <stdexcept>

#include "bt/csv.hpp"
#include "bt/synthetic.hpp"
#include "doctest.h"
#include "test_util.hpp"

using namespace tu;

// =====================================================================================================
// Scenario 1: one complete round trip, every number computed by hand (costs = hand_costs()).
// =====================================================================================================
TEST_CASE("engine: hand-computed buy/sell round trip, to the paisa") {
  // Bars (open, high, low, close):
  //   idx0 d100  995 1005  990 1000   <- strategy buys 100 at this close
  //   idx1 d101 1000 1010  998 1010   <- buy fills at the OPEN 1000
  //   idx2 d102 1010 1020 1005 1015   <- strategy sells 100 at this close
  //   idx3 d103 1010 1015 1005 1012   <- sell fills at the OPEN 1010
  //   idx4 d104 1012 1020 1010 1015
  const AlignedData data = one_symbol({mk(100, 995, 1005, 990, 1000), mk(101, 1000, 1010, 998, 1010),
                                       mk(102, 1010, 1020, 1005, 1015), mk(103, 1010, 1015, 1005, 1012),
                                       mk(104, 1012, 1020, 1010, 1015)});
  Scripted s([](Context& c) {
    if (c.day_index() == 0) c.buy(0, 100);
    if (c.day_index() == 2) c.sell(0, 100);
  });
  const EngineResult r = Engine(data, engine_cfg(20'000'000, hand_costs())).run(s);  // Rs 2,00,000

  REQUIRE(r.fills.size() == 2);
  // BUY: notional 100 * 1000 = 10,000,000 paise; fees 14,226 (see test_money_costs)
  const Fill& b = r.fills[0];
  CHECK(b.order_id == 1);
  CHECK(b.day_index == 1);
  CHECK(b.date == 101);
  CHECK(b.qty == 100);
  CHECK(b.price == 100000);
  CHECK(b.notional == 10'000'000);
  CHECK(b.fees.total() == 14226);
  CHECK(b.cash_after == 20'000'000 - 10'000'000 - 14'226);  // 9,985,774
  CHECK(b.position_after == 100);
  // SELL: notional 100 * 1010 = 10,100,000; fees 12,829
  const Fill& sl = r.fills[1];
  CHECK(sl.order_id == 2);
  CHECK(sl.day_index == 3);
  CHECK(sl.price == 101000);
  CHECK(sl.notional == 10'100'000);
  CHECK(sl.fees.total() == 12829);
  CHECK(sl.cash_after == 9'985'774 + 10'100'000 - 12'829);  // 20,072,945
  CHECK(sl.position_after == 0);

  // Daily equity = cash + qty * close, marked after that day's fills.
  REQUIRE(r.equity_curve.size() == 5);
  CHECK(r.equity_curve[0].equity == 20'000'000);
  CHECK(r.equity_curve[1].cash == 9'985'774);
  CHECK(r.equity_curve[1].positions_value == 100 * 101'000);        // close 1010
  CHECK(r.equity_curve[1].equity == 9'985'774 + 10'100'000);        // 20,085,774
  CHECK(r.equity_curve[2].equity == 9'985'774 + 100 * 101'500);     // close 1015 -> 20,135,774
  CHECK(r.equity_curve[3].positions_value == 0);
  CHECK(r.equity_curve[3].equity == 20'072'945);
  CHECK(r.equity_curve[4].equity == 20'072'945);

  // Round-trip trade.
  REQUIRE(r.trades.size() == 1);
  const Trade& t = r.trades[0];
  CHECK(t.direction == Side::Buy);
  CHECK(t.entry_day == 1);
  CHECK(t.exit_day == 3);
  CHECK(t.qty == 100);
  CHECK(t.entry_value == 10'000'000);
  CHECK(t.exit_value == 10'100'000);
  CHECK(t.gross_pnl == 100'000);              // (1010 - 1000) * 100 shares = Rs 1,000
  CHECK(t.fees.total() == 27'055);            // 14,226 + 12,829
  CHECK(t.net_pnl == 72'945);
  CHECK(t.num_fills == 2);

  // Totals by component: brokerage 2*2000, STT 10,000+10,100, exchange 300+303, SEBI 10+10, stamp 1,500, GST 416+416.
  CHECK(r.fees.brokerage == 4000);
  CHECK(r.fees.stt == 20100);
  CHECK(r.fees.exchange == 603);
  CHECK(r.fees.sebi == 20);
  CHECK(r.fees.stamp == 1500);
  CHECK(r.fees.gst == 832);
  CHECK(r.fees.total() == 27'055);
  CHECK(r.realized_gross == 100'000);
  CHECK(r.final_cash == 20'072'945);
  CHECK(r.final_equity == 20'072'945);
  CHECK(r.final_cash - r.initial_cash == t.net_pnl);  // cash change == net P&L when flat
  CHECK(r.open_positions.empty());
  CHECK(r.orders_pending_at_end == 0);
}

TEST_CASE("engine: fixed-bps slippage on market fills (hand-computed)") {
  // idx1 open 1000 -> buy pays 1000 + 10 bps = 1001.00 -> 100,100 paise/share; notional 10,010,000.
  // idx2 open 1010 -> sell receives 1010 - 1.01 = 1008.99 -> 100,899 paise/share; notional 10,089,900.
  const AlignedData data = one_symbol({mk(100, 1000, 1010, 990, 1000), mk(101, 1000, 1010, 990, 1000),
                                       mk(102, 1010, 1020, 1000, 1010), mk(103, 1010, 1020, 1000, 1010)});
  Scripted s([](Context& c) {
    if (c.day_index() == 0) c.buy(0, 100);
    if (c.day_index() == 1) c.sell(0, 100);
  });
  EngineConfig c = engine_cfg(20'000'000, free_costs());
  c.slippage.mode = SlippageConfig::Mode::FixedBps;
  c.slippage.value = 10;
  const EngineResult r = Engine(data, c).run(s);
  REQUIRE(r.fills.size() == 2);
  CHECK(r.fills[0].price == 100'100);
  CHECK(r.fills[0].notional == 10'010'000);
  CHECK(r.fills[1].price == 100'899);
  CHECK(r.fills[1].notional == 10'089'900);
  CHECK(r.final_cash == 20'000'000 - 10'010'000 + 10'089'900);  // 20,079,900
}

// =====================================================================================================
// Cash rejection
// =====================================================================================================
TEST_CASE("engine: cash check includes fees, at the paisa boundary") {
  const AlignedData data = one_symbol(flat_bars(3));
  auto run = [&](Paise cash) {
    Scripted s([](Context& c) { if (c.day_index() == 0) c.buy(0, 100); });
    return Engine(data, engine_cfg(cash, hand_costs())).run(s);
  };
  // Needed: 10,000,000 notional + 14,226 fees = 10,014,226.
  const EngineResult ok = run(10'014'226);
  REQUIRE(ok.fills.size() == 1);
  CHECK(ok.fills[0].cash_after == 0);
  CHECK(ok.events.empty());

  const EngineResult no = run(10'014'225);
  CHECK(no.fills.empty());
  REQUIRE(no.events.size() == 1);
  CHECK(no.events[0].kind == OrderEventKind::Rejected);
  CHECK(no.events[0].reason == RejectReason::InsufficientCash);
  CHECK(no.events[0].id == 1);
  CHECK(no.final_cash == 10'014'225);  // untouched
  CHECK(no.open_positions.empty());
  CHECK(no.orders_pending_at_end == 0);  // a rejected order is removed, not retried
}

TEST_CASE("engine: long-only sell with no / too little position is rejected, never an implicit short") {
  const AlignedData data = one_symbol(flat_bars(4));
  Scripted s([](Context& c) {
    if (c.day_index() == 0) {
      c.sell(0, 10);  // no position at all
      c.buy(0, 5);
    }
    if (c.day_index() == 1) c.sell(0, 6);  // more than the 5 held
  });
  const EngineResult r = Engine(data, engine_cfg(10'000'000, free_costs())).run(s);
  // The id-1 sell is processed after the id-2 buy? No: both are Market, ascending id: sell (id1) first -> rejected.
  REQUIRE(r.events.size() == 2);
  CHECK(r.events[0].id == 1);
  CHECK(r.events[0].reason == RejectReason::ShortingDisabled);
  CHECK(r.events[1].id == 3);
  CHECK(r.events[1].reason == RejectReason::ShortingDisabled);
  REQUIRE(r.fills.size() == 1);
  CHECK(r.fills[0].side == Side::Buy);
  REQUIRE(r.open_positions.size() == 1);
  CHECK(r.open_positions[0].qty == 5);
}

// =====================================================================================================
// Order lifecycle
// =====================================================================================================
TEST_CASE("lifecycle: an order never fills on the bar it was submitted; it uses the NEXT bar's open") {
  // idx0 open 1000, idx1 open 1050. Decision at idx0's close -> fill at idx1's open (1050), not idx0's.
  const AlignedData data = one_symbol({mk(100, 1000, 1010, 990, 1000), mk(101, 1050, 1060, 1040, 1055)});
  Scripted s([](Context& c) { if (c.day_index() == 0) c.buy(0, 1); });
  const EngineResult r = Engine(data, engine_cfg(10'000'000, free_costs())).run(s);
  REQUIRE(r.fills.size() == 1);
  CHECK(r.fills[0].day_index == 1);
  CHECK(r.fills[0].price == 105'000);
}

TEST_CASE("lifecycle: Day orders live for exactly one bar; GTC orders wait") {
  // Limit buy at 960. idx1 low 990 (not touched), idx2 low 950 (touched).
  const AlignedData data = one_symbol({mk(100, 1000, 1010, 990, 1000), mk(101, 1000, 1010, 990, 1000),
                                       mk(102, 1000, 1010, 950, 1000), mk(103, 1000, 1010, 990, 1000)});
  {
    Scripted s([](Context& c) { if (c.day_index() == 0) c.limit(0, Side::Buy, 10, 960, TimeInForce::Day); });
    const EngineResult r = Engine(data, engine_cfg(10'000'000, free_costs())).run(s);
    CHECK(r.fills.empty());                       // expired at idx1 and so never saw idx2
    REQUIRE(r.events.size() == 1);
    CHECK(r.events[0].kind == OrderEventKind::Expired);
    CHECK(r.events[0].date == 101);
    CHECK(r.orders_pending_at_end == 0);
  }
  {
    Scripted s([](Context& c) { if (c.day_index() == 0) c.limit(0, Side::Buy, 10, 960, TimeInForce::GTC); });
    const EngineResult r = Engine(data, engine_cfg(10'000'000, free_costs())).run(s);
    REQUIRE(r.fills.size() == 1);
    CHECK(r.fills[0].day_index == 2);
    CHECK(r.fills[0].price == 96'000);            // min(open 1000, limit 960)
    CHECK(r.events.empty());
  }
}

TEST_CASE("lifecycle: a symbol with no bar today cannot fill; Day orders still expire, GTC orders wait") {
  // A trades every day (it defines the calendar); Y has a hole on d101.
  const AlignedData data = align_calendar({
      {"A", {mk(100, 10, 11, 9, 10), mk(101, 10, 11, 9, 10), mk(102, 10, 11, 9, 10), mk(103, 10, 11, 9, 10)}},
      {"Y", {mk(100, 1000, 1010, 990, 1000), mk(102, 1020, 1030, 1010, 1020), mk(103, 1020, 1030, 1010, 1020)}}});
  Scripted s([](Context& c) {
    if (c.day_index() == 0) {
      c.market(1, Side::Buy, 1, TimeInForce::Day);  // id 1
      c.market(1, Side::Buy, 1, TimeInForce::GTC);  // id 2
    }
  });
  const EngineResult r = Engine(data, engine_cfg(10'000'000, free_costs())).run(s);
  REQUIRE(r.events.size() == 1);
  CHECK(r.events[0].kind == OrderEventKind::Expired);
  CHECK(r.events[0].id == 1);
  CHECK(r.events[0].date == 101);
  REQUIRE(r.fills.size() == 1);
  CHECK(r.fills[0].order_id == 2);
  CHECK(r.fills[0].day_index == 2);
  CHECK(r.fills[0].price == 102'000);  // d102's open
}

TEST_CASE("lifecycle: mark-to-market on a day the held symbol has no bar uses its last real close") {
  const AlignedData data = align_calendar({
      {"A", {mk(100, 10, 11, 9, 10), mk(101, 10, 11, 9, 10), mk(102, 10, 11, 9, 10), mk(103, 10, 11, 9, 10)}},
      {"Y", {mk(100, 1000, 1010, 990, 1000), mk(101, 1000, 1020, 990, 1005), mk(103, 1030, 1040, 1020, 1035)}}});
  Scripted s([](Context& c) { if (c.day_index() == 0) c.buy(1, 10); });
  const EngineResult r = Engine(data, engine_cfg(10'000'000, free_costs())).run(s);
  // Bought 10 @ 1000 on idx1. idx2 has no Y bar: value at the d101 close 1005, NOT forward-filled into a fill.
  CHECK(r.equity_curve[1].positions_value == 10 * 100'500);
  CHECK(r.equity_curve[2].positions_value == 10 * 100'500);
  CHECK(r.equity_curve[3].positions_value == 10 * 103'500);
  CHECK(r.fills.size() == 1);
}

TEST_CASE("lifecycle: orders submitted on the final bar never fill; open positions are not liquidated") {
  const AlignedData data = one_symbol(flat_bars(3, 1000, 1010, 990, 1000));
  Scripted s([](Context& c) {
    if (c.day_index() == 0) c.buy(0, 10);  // fills at idx1
    if (c.day_index() == 2) c.sell(0, 10);  // submitted on the last bar
  });
  const EngineResult r = Engine(data, engine_cfg(10'000'000, free_costs())).run(s);
  REQUIRE(r.fills.size() == 1);
  CHECK(r.orders_pending_at_end == 1);
  REQUIRE(r.open_positions.size() == 1);
  CHECK(r.open_positions[0].qty == 10);
  CHECK(r.open_positions[0].mark == 100'000);
  CHECK(r.open_positions[0].cost == 10 * 100'000);
  CHECK(r.open_positions[0].unrealized_gross == 0);
  CHECK(r.final_equity == r.final_cash + 10 * 100'000);  // marked, not sold
  CHECK(r.trades.empty());                                // no closed round trip
}

TEST_CASE("lifecycle: cancel, cancel of a staged order, cancel_all, unknown ids, deterministic ids") {
  const AlignedData data = one_symbol({mk(100, 1000, 1010, 990, 1000), mk(101, 1000, 1010, 990, 1000),
                                       mk(102, 1000, 1010, 890, 1000), mk(103, 1000, 1010, 990, 1000)});
  std::vector<OrderId> ids;
  bool staged_cancel = false, unknown_cancel = true;
  std::size_t all_cancelled = 0;
  Scripted s([&](Context& c) {
    if (c.day_index() == 0) {
      ids.push_back(c.limit(0, Side::Buy, 10, 900, TimeInForce::GTC));  // id 1: would fill at idx2 (low 890)
      const OrderId tmp = c.limit(0, Side::Buy, 10, 900, TimeInForce::GTC);  // id 2
      ids.push_back(tmp);
      staged_cancel = c.cancel(tmp);  // cancelled while still staged
    }
    if (c.day_index() == 1) {
      unknown_cancel = c.cancel(999);
      CHECK(c.open_orders(0).size() == 1);    // only id 1 is still pending
      ids.push_back(c.limit(0, Side::Buy, 5, 880, TimeInForce::GTC));  // id 3 - ids keep counting
      all_cancelled = c.cancel_all(0);        // id 1 and id 3
      CHECK(c.open_orders(0).empty());
    }
  });
  const EngineResult r = Engine(data, engine_cfg(10'000'000, free_costs())).run(s);
  CHECK(ids == std::vector<OrderId>{1, 2, 3});
  CHECK(staged_cancel);
  CHECK_FALSE(unknown_cancel);
  CHECK(all_cancelled == 2);
  CHECK(r.fills.empty());  // the touched limit was cancelled before it could fill
  CHECK(count_events(r, OrderEventKind::Cancelled) == 3);
}

TEST_CASE("lifecycle: invalid orders are programming errors") {
  const AlignedData data = one_symbol(flat_bars(2));
  Scripted s([](Context& c) {
    CHECK_THROWS_AS(c.buy(0, 0), std::invalid_argument);
    CHECK_THROWS_AS(c.buy(0, -5), std::invalid_argument);
    CHECK_THROWS_AS(c.buy(7, 1), std::invalid_argument);
    CHECK_THROWS_AS(c.limit(0, Side::Buy, 1, 0.0), std::invalid_argument);
    CHECK_THROWS_AS(c.limit(0, Side::Buy, 1, -3.0), std::invalid_argument);
    CHECK_THROWS_AS(c.stop(0, Side::Sell, 1, std::nan("")), std::invalid_argument);
    CHECK_THROWS_AS(c.limit(0, Side::Buy, 1, 0.001), std::invalid_argument);  // rounds to zero paise
  });
  Engine(data, engine_cfg(1'000'000, free_costs())).run(s);
}

TEST_CASE("engine: a bar whose price rounds to 0 paise is rejected up front, naming symbol and date") {
  // Rs 0.004 passes the loader (positive) but is 0 paise inside the engine.
  const AlignedData bad = one_symbol({mk(100, 10, 11, 9, 10), mk(101, 0.004, 0.006, 0.004, 0.005)}, "PENNY");
  Scripted s([](Context&) {});
  try {
    Engine(bad, engine_cfg(1'000'000, free_costs())).run(s);
    FAIL("should have thrown");
  } catch (const std::invalid_argument& e) {
    const std::string m = e.what();
    CHECK(m.find("PENNY") != std::string::npos);
    CHECK(m.find(format_date(101)) != std::string::npos);
  }
  // Rs 0.01 is exactly 1 paisa and is fine; so is Rs 0.005 -> rounds to 1 paisa.
  const AlignedData ok = one_symbol({mk(100, 0.01, 0.02, 0.01, 0.01), mk(101, 0.01, 0.02, 0.005, 0.01)});
  CHECK_NOTHROW(Engine(ok, engine_cfg(1'000'000, free_costs())).run(s));
}

TEST_CASE("lifecycle: phases - nothing to trade or see in on_start, no orders in on_end") {
  const AlignedData data = one_symbol(flat_bars(3));
  bool start_ran = false, end_ran = false;
  Scripted s([](Context&) {});
  s.start_fn = [&](Context& c) {
    start_ran = true;
    CHECK(c.phase() == Phase::Start);
    CHECK(c.history(0).empty());
    CHECK_FALSE(c.traded_today(0));
    CHECK(c.num_symbols() == 1);
    CHECK(c.cash_paise() == 1'000'000);
    CHECK_THROWS_AS(c.date(), std::logic_error);
    CHECK_THROWS_AS(c.day_index(), std::logic_error);
    CHECK_THROWS_AS(c.buy(0, 1), std::logic_error);
  };
  s.end_fn = [&](Context& c) {
    end_ran = true;
    CHECK(c.phase() == Phase::End);
    CHECK(c.history(0).size() == 3);
    CHECK_THROWS_AS(c.buy(0, 1), std::logic_error);
    CHECK_THROWS_AS(c.cancel(1), std::logic_error);
  };
  Engine(data, engine_cfg(1'000'000, free_costs())).run(s);
  CHECK(start_ran);
  CHECK(end_ran);
}

TEST_CASE("history view: shows exactly the bars up to today, and throws beyond") {
  // Y lists late (first bar d102).
  const AlignedData data = align_calendar({
      {"A", {mk(100, 10, 11, 9, 10), mk(101, 11, 12, 10, 11), mk(102, 12, 13, 11, 12), mk(103, 13, 14, 12, 13)}},
      {"Y", {mk(102, 50, 51, 49, 50), mk(103, 51, 52, 50, 51)}}});
  Scripted s([](Context& c) {
    const std::size_t t = c.day_index();
    const HistoryView a = c.history(0);
    const HistoryView y = c.history(1);
    CHECK(a.size() == t + 1);
    CHECK(a[0].date == static_cast<Date>(100 + t));          // [0] is today
    CHECK(a[t].date == 100);                                  // oldest visible
    CHECK_THROWS_AS(a[t + 1], std::out_of_range);             // one past the oldest: tomorrow is unreachable
    CHECK_THROWS_AS(a.at(1000), std::out_of_range);
    CHECK(y.size() == (t >= 2 ? t - 1 : 0));                  // nothing before its listing
    if (t < 2) {
      CHECK(y.empty());
      CHECK_THROWS_AS(y.latest(), std::out_of_range);
    }
  });
  Engine(data, engine_cfg(1'000'000, free_costs())).run(s);
}

TEST_CASE("history view: on a day the symbol did not trade, [0] is its last real bar") {
  const AlignedData data = align_calendar({
      {"A", {mk(100, 10, 11, 9, 10), mk(101, 10, 11, 9, 10), mk(102, 10, 11, 9, 10)}},
      {"Y", {mk(100, 50, 51, 49, 50), mk(102, 52, 53, 51, 52)}}});
  Scripted s([](Context& c) {
    if (c.day_index() == 1) {
      CHECK_FALSE(c.traded_today(1));
      CHECK(c.history(1).size() == 1);
      CHECK(c.history(1)[0].date == 100);
    }
    if (c.day_index() == 2) CHECK(c.traded_today(1));
  });
  Engine(data, engine_cfg(1'000'000, free_costs())).run(s);
}

TEST_CASE("lifecycle: volume participation cap - partial fills, Day remainder expires, GTC continues") {
  const AlignedData data = one_symbol(flat_bars(8, 1000, 1010, 990, 1000, 1000));  // volume 1000 per bar
  EngineConfig c = engine_cfg(1'000'000'000, free_costs());
  c.max_volume_participation = 0.1;  // at most 100 shares per bar
  {
    Scripted s([](Context& ctx) { if (ctx.day_index() == 0) ctx.market(0, Side::Buy, 500, TimeInForce::Day); });
    const EngineResult r = Engine(data, c).run(s);
    REQUIRE(r.fills.size() == 1);
    CHECK(r.fills[0].qty == 100);
    CHECK(count_events(r, OrderEventKind::Expired) == 1);  // the 400-share remainder
    CHECK(r.open_positions[0].qty == 100);
  }
  {
    Scripted s([](Context& ctx) { if (ctx.day_index() == 0) ctx.market(0, Side::Buy, 500, TimeInForce::GTC); });
    const EngineResult r = Engine(data, c).run(s);
    REQUIRE(r.fills.size() == 5);
    for (std::size_t i = 0; i < 5; ++i) {
      CHECK(r.fills[i].qty == 100);
      CHECK(r.fills[i].order_id == 1);
      CHECK(r.fills[i].day_index == i + 1);
    }
    CHECK(r.open_positions[0].qty == 500);
    CHECK(r.orders_pending_at_end == 0);
    REQUIRE(r.trades.empty());  // still open
  }
}

// =====================================================================================================
// Processing order within a bar
// =====================================================================================================
TEST_CASE("order: when a stop and a limit are both touched in one bar, the STOP is processed first") {
  // idx2 bar spans 970..1030, so a take-profit sell limit at 1020 and a stop-loss sell at 980 are both hit.
  const AlignedData data = one_symbol({mk(100, 1000, 1010, 990, 1000), mk(101, 1000, 1010, 990, 1000),
                                       mk(102, 1000, 1030, 970, 1000), mk(103, 1000, 1010, 990, 1000)});
  OrderId limit_id = 0, stop_id = 0;
  Scripted s([&](Context& c) {
    if (c.day_index() == 0) c.buy(0, 100);  // id 1, fills idx1 at 1000
    if (c.day_index() == 1) {
      limit_id = c.limit(0, Side::Sell, 100, 1020, TimeInForce::GTC);  // id 2 (lower id than the stop)
      stop_id = c.stop(0, Side::Sell, 100, 980, TimeInForce::GTC);     // id 3
    }
  });
  const EngineResult r = Engine(data, engine_cfg(20'000'000, free_costs())).run(s);
  REQUIRE(r.fills.size() == 2);
  CHECK(r.fills[1].order_id == stop_id);
  CHECK(r.fills[1].price == 98'000);  // min(open 1000, stop 980)
  REQUIRE(r.events.size() == 1);
  CHECK(r.events[0].id == limit_id);
  CHECK(r.events[0].kind == OrderEventKind::Rejected);
  CHECK(r.events[0].reason == RejectReason::ShortingDisabled);  // position already closed; no OCO linkage
  REQUIRE(r.trades.size() == 1);
  CHECK(r.trades[0].gross_pnl == (98'000 - 100'000) * 100);  // -Rs 2,000
}

TEST_CASE("order: an entry and its protective stop submitted together both execute on the same bar") {
  // The stop is submitted FIRST (id 1), the market entry second (id 2); rank order still runs the entry first.
  // idx1: open 1000, low 940 -> entry fills at 1000, then the stop (950) triggers at min(open, 950) = 950.
  const AlignedData data = one_symbol({mk(100, 1000, 1010, 990, 1000), mk(101, 1000, 1010, 940, 1000),
                                       mk(102, 1000, 1010, 990, 1000)});
  Scripted s([](Context& c) {
    if (c.day_index() == 0) {
      c.stop(0, Side::Sell, 100, 950, TimeInForce::GTC);  // id 1
      c.buy(0, 100);                                       // id 2
    }
  });
  const EngineResult r = Engine(data, engine_cfg(20'000'000, free_costs())).run(s);
  REQUIRE(r.fills.size() == 2);
  CHECK(r.fills[0].order_id == 2);
  CHECK(r.fills[0].price == 100'000);
  CHECK(r.fills[1].order_id == 1);
  CHECK(r.fills[1].price == 95'000);
  CHECK(r.events.empty());
  REQUIRE(r.trades.size() == 1);
  CHECK(r.trades[0].gross_pnl == -500'000);  // (950 - 1000) * 100 = -Rs 5,000
  CHECK(r.trades[0].entry_day == 1);
  CHECK(r.trades[0].exit_day == 1);
}

TEST_CASE("order: a gap through a stop fills at the open, end to end") {
  const AlignedData data = one_symbol({mk(100, 1000, 1010, 990, 1000), mk(101, 1000, 1010, 990, 1000),
                                       mk(102, 900, 920, 880, 910), mk(103, 910, 920, 900, 910)});
  Scripted s([](Context& c) {
    if (c.day_index() == 0) c.buy(0, 10);
    if (c.day_index() == 1) c.stop(0, Side::Sell, 10, 950, TimeInForce::GTC);
  });
  const EngineResult r = Engine(data, engine_cfg(20'000'000, free_costs())).run(s);
  REQUIRE(r.fills.size() == 2);
  CHECK(r.fills[1].price == 90'000);  // opened at 900, below the 950 stop
  CHECK(r.trades[0].gross_pnl == (90'000 - 100'000) * 10);
}

TEST_CASE("order: a gap through a buy limit fills at the (better) open, end to end") {
  const AlignedData data = one_symbol({mk(100, 1000, 1010, 990, 1000), mk(101, 900, 920, 880, 910),
                                       mk(102, 910, 920, 900, 910)});
  Scripted s([](Context& c) { if (c.day_index() == 0) c.limit(0, Side::Buy, 10, 950, TimeInForce::GTC); });
  const EngineResult r = Engine(data, engine_cfg(20'000'000, free_costs())).run(s);
  REQUIRE(r.fills.size() == 1);
  CHECK(r.fills[0].price == 90'000);  // not the 950 limit
}

// =====================================================================================================
// Shorting
// =====================================================================================================
namespace {
AlignedData short_data() {
  return one_symbol({mk(100, 1000, 1010, 990, 1000), mk(101, 1000, 1005, 985, 990), mk(102, 990, 1000, 980, 985),
                     mk(103, 950, 960, 940, 955)});
}
}  // namespace

TEST_CASE("shorting: flag off -> rejected") {
  const AlignedData data = short_data();
  Scripted s([](Context& c) { if (c.day_index() == 0) c.sell(0, 100); });
  const EngineResult r = Engine(data, engine_cfg(20'000'000, hand_costs())).run(s);
  CHECK(r.fills.empty());
  REQUIRE(r.events.size() == 1);
  CHECK(r.events[0].reason == RejectReason::ShortingDisabled);
  CHECK(r.final_cash == 20'000'000);
}

TEST_CASE("shorting: flag on, one hand-computed round trip") {
  // idx1 open 1000: SHORT 100.  notional 10,000,000.  fees (sell side):
  //   brokerage 2,000; STT 10,000; exchange 300; SEBI 10; stamp 0; GST 18 % * 2,310 = 415.8 -> 416; total 12,726
  // idx3 open 950: COVER 100.  notional 9,500,000.  fees (buy side):
  //   brokerage 2,000; STT 9,500; exchange 285; SEBI 9.5 -> 10; stamp 1,425; GST 18 % * 2,295 = 413.1 -> 413; total 13,633
  // gross = 10,000,000 - 9,500,000 = 500,000;  fees = 26,359;  net = 473,641.
  const AlignedData data = short_data();
  Scripted s([](Context& c) {
    if (c.day_index() == 0) c.sell(0, 100);
    if (c.day_index() == 2) c.buy(0, 100);
  });
  EngineConfig c = engine_cfg(20'000'000, hand_costs());
  c.allow_short = true;
  const EngineResult r = Engine(data, c).run(s);
  REQUIRE(r.fills.size() == 2);
  CHECK(r.fills[0].fees.total() == 12'726);
  CHECK(r.fills[0].cash_after == 20'000'000 + 10'000'000 - 12'726);  // 29,987,274
  CHECK(r.fills[0].position_after == -100);
  CHECK(r.fills[1].fees.total() == 13'633);
  CHECK(r.fills[1].position_after == 0);
  CHECK(r.final_cash == 20'473'641);
  // While short, equity = cash + qty*close = 29,987,274 - 100 * 99,000 (close 990 on idx1).
  CHECK(r.equity_curve[1].positions_value == -9'900'000);
  CHECK(r.equity_curve[1].equity == 29'987'274 - 9'900'000);  // 20,087,274
  REQUIRE(r.trades.size() == 1);
  const Trade& t = r.trades[0];
  CHECK(t.direction == Side::Sell);
  CHECK(t.entry_value == 10'000'000);
  CHECK(t.exit_value == 9'500'000);
  CHECK(t.gross_pnl == 500'000);
  CHECK(t.fees.total() == 26'359);
  CHECK(t.net_pnl == 473'641);
  CHECK(r.realized_gross == 500'000);
}

TEST_CASE("shorting: a single fill may not cross zero (either way) - rejected CrossesZero") {
  EngineConfig c = engine_cfg(50'000'000, free_costs());
  c.allow_short = true;
  {  // long 100, then sell 150
    const AlignedData data = one_symbol(flat_bars(4));
    Scripted s([](Context& ctx) {
      if (ctx.day_index() == 0) ctx.buy(0, 100);
      if (ctx.day_index() == 1) ctx.sell(0, 150);
    });
    const EngineResult r = Engine(data, c).run(s);
    REQUIRE(r.events.size() == 1);
    CHECK(r.events[0].reason == RejectReason::CrossesZero);
    CHECK(r.open_positions[0].qty == 100);
  }
  {  // short 100, then buy 150
    const AlignedData data = one_symbol(flat_bars(4));
    Scripted s([](Context& ctx) {
      if (ctx.day_index() == 0) ctx.sell(0, 100);
      if (ctx.day_index() == 1) ctx.buy(0, 150);
    });
    const EngineResult r = Engine(data, c).run(s);
    REQUIRE(r.events.size() == 1);
    CHECK(r.events[0].reason == RejectReason::CrossesZero);
    CHECK(r.open_positions[0].qty == -100);
  }
  {  // flipping is done as two orders: close, then open the other way (ascending ids at the same open)
    const AlignedData data = one_symbol(flat_bars(4));
    Scripted s([](Context& ctx) {
      if (ctx.day_index() == 0) ctx.buy(0, 100);
      if (ctx.day_index() == 1) {
        ctx.sell(0, 100);
        ctx.sell(0, 50);
      }
    });
    const EngineResult r = Engine(data, c).run(s);
    CHECK(r.events.empty());
    REQUIRE(r.open_positions.size() == 1);
    CHECK(r.open_positions[0].qty == -50);
    CHECK(r.trades.size() == 1);  // the long round trip closed; the short is still open
  }
}

// =====================================================================================================
// Reconciliation and invariants on random activity
// =====================================================================================================
namespace {

AlignedData random_universe(std::uint64_t seed) {
  std::mt19937_64 rng(seed * 7919 + 13);
  auto drop = [&](std::vector<Bar> bars, double p, std::size_t keep_last) {
    std::vector<Bar> out;
    for (std::size_t i = 0; i < bars.size(); ++i) {
      const bool protect = i + keep_last >= bars.size();
      if (protect || static_cast<double>(rng() % 1000) / 1000.0 >= p) out.push_back(bars[i]);
    }
    return out;
  };
  SynthParams p;
  p.start = parse_date("2022-01-03").value();
  p.num_bars = 300;
  p.seed = seed * 3 + 1;
  p.start_price = 100;
  auto a = generate_gbm(p);
  p.seed = seed * 3 + 2;
  p.start_price = 500;
  auto b = drop(generate_gbm(p), 0.12, 20);  // holes
  p.seed = seed * 3 + 3;
  p.start_price = 2000;
  auto c = generate_gbm(p);
  c.erase(c.begin(), c.begin() + 40);  // lists late
  return align_calendar({{"A", a}, {"B", b}, {"C", c}});
}

// Trades randomly (all order types), cancels at random, and flattens in the last 10 days.
struct RandomStrategy : Strategy {
  std::mt19937_64 rng;
  std::size_t n_days;
  explicit RandomStrategy(std::uint64_t seed, std::size_t days) : rng(seed), n_days(days) {}
  void on_bar(Context& c) override {
    const std::size_t t = c.day_index();
    for (std::size_t s = 0; s < c.num_symbols(); ++s) {
      const HistoryView h = c.history(s);
      if (h.empty()) continue;
      const double px = h.latest().close;
      if (t + 10 == n_days) {  // flatten: cancel everything, sell what is held with a GTC order
        c.cancel_all(s);
        if (c.position(s) > 0) c.market(s, Side::Sell, c.position(s), TimeInForce::GTC);
        continue;
      }
      if (t + 10 > n_days) continue;  // quiet period so everything settles
      if (rng() % 100 >= 40) continue;
      const std::int64_t qty = 1 + static_cast<std::int64_t>(rng() % 200);
      const double u = static_cast<double>(rng() % 1000) / 1000.0;
      const TimeInForce tif = rng() % 2 ? TimeInForce::GTC : TimeInForce::Day;
      switch (rng() % 7) {
        case 0: c.market(s, Side::Buy, qty, tif); break;
        case 1: c.limit(s, Side::Buy, qty, px * (0.90 + 0.10 * u), tif); break;
        case 2: c.stop(s, Side::Buy, qty, px * (1.00 + 0.10 * u), tif); break;
        case 3: if (c.position(s) > 0) c.market(s, Side::Sell, 1 + static_cast<std::int64_t>(rng() % static_cast<std::uint64_t>(c.position(s))), tif); break;
        case 4: c.limit(s, Side::Sell, qty, px * (1.00 + 0.10 * u), tif); break;
        case 5: c.stop(s, Side::Sell, qty, px * (0.90 + 0.10 * u), tif); break;
        default: c.cancel_all(s); break;
      }
    }
  }
};

EngineConfig random_cfg() {
  EngineConfig c;
  c.initial_cash = to_paise(100'000.0);  // small enough that many buys get rejected
  c.costs = CostConfig::nse_delivery_defaults();
  c.slippage.mode = SlippageConfig::Mode::FixedBps;
  c.slippage.value = 5;
  return c;
}

// Re-derives cash, positions and equity from the fill log alone, with code that shares nothing with Portfolio.
void replay_and_compare(const EngineResult& r, const AlignedData& data, const EngineConfig& cfg) {
  Paise cash = r.initial_cash;
  std::vector<std::int64_t> pos(data.num_symbols(), 0);
  FeeBreakdown fee_sum;
  for (const Fill& f : r.fills) {
    REQUIRE(f.notional == f.price * f.qty);
    REQUIRE(f.fees == compute_fees(cfg.costs, f.side, f.notional));
    cash += (f.side == Side::Sell ? f.notional : -f.notional) - f.fees.total();
    pos[f.symbol] += f.side == Side::Buy ? f.qty : -f.qty;
    fee_sum += f.fees;
    REQUIRE(cash >= 0);
    REQUIRE(cash == f.cash_after);
    REQUIRE(pos[f.symbol] >= 0);  // long-only
    REQUIRE(pos[f.symbol] == f.position_after);
  }
  CHECK(cash == r.final_cash);
  CHECK(fee_sum == r.fees);

  Paise mv = 0, unreal = 0;
  for (std::size_t s = 0; s < pos.size(); ++s) {
    const Bar* last = data.last_bar(s, data.num_days() - 1);
    if (pos[s] != 0) {
      REQUIRE(last != nullptr);
      mv += pos[s] * to_paise(last->close);
    }
  }
  for (const OpenPosition& op : r.open_positions) {
    REQUIRE(op.qty == pos[op.symbol]);
    unreal += op.unrealized_gross;
  }
  CHECK(r.final_equity == cash + mv);
  CHECK(r.equity_curve.back().equity == r.final_equity);
  // equity - initial == realised + unrealised - fees   (exact, in paise)
  CHECK(r.final_equity - r.initial_cash == r.realized_gross + unreal - r.fees.total());

  for (const DailyRecord& d : r.equity_curve) {
    REQUIRE(d.cash >= 0);
    REQUIRE(d.equity == d.cash + d.positions_value);
  }
}

}  // namespace

TEST_CASE("reconciliation: independent replay of the fill log matches the engine, on random activity") {
  std::size_t total_fills = 0, total_trades = 0, cash_rejects = 0, expiries = 0, cancels = 0;
  for (std::uint64_t seed = 1; seed <= 8; ++seed) {
    CAPTURE(seed);
    const AlignedData data = random_universe(seed);
    RandomStrategy strat(seed, data.num_days());
    const EngineConfig c = random_cfg();
    const EngineResult r = Engine(data, c).run(strat);
    replay_and_compare(r, data, c);

    // Everything was flattened in the last 10 days, so the trade log must account for ALL cash movement.
    CHECK(r.open_positions.empty());
    Paise net = 0, gross = 0;
    for (const Trade& t : r.trades) {
      net += t.net_pnl;
      gross += t.gross_pnl;
      CHECK(t.net_pnl == t.gross_pnl - t.fees.total());
      CHECK(t.exit_day >= t.entry_day);
    }
    CHECK(net == r.final_cash - r.initial_cash);
    CHECK(gross == r.realized_gross);

    total_fills += r.fills.size();
    total_trades += r.trades.size();
    for (const auto& e : r.events) {
      if (e.kind == OrderEventKind::Rejected && e.reason == RejectReason::InsufficientCash) ++cash_rejects;
      if (e.kind == OrderEventKind::Expired) ++expiries;
      if (e.kind == OrderEventKind::Cancelled) ++cancels;
    }
  }
  // Guard against a vacuous test: the random activity really exercised these paths.
  CHECK(total_fills > 100);
  CHECK(total_trades > 20);
  CHECK(cash_rejects > 0);
  CHECK(expiries > 0);
  CHECK(cancels > 0);
}

TEST_CASE("determinism: the same strategy on the same data gives byte-identical results") {
  const AlignedData data = random_universe(5);
  auto run = [&]() {
    RandomStrategy strat(5, data.num_days());  // fresh strategy per run
    return Engine(data, random_cfg()).run(strat);
  };
  const EngineResult a = run(), b = run();
  REQUIRE(a.fills.size() == b.fills.size());
  for (std::size_t i = 0; i < a.fills.size(); ++i) {
    CHECK(a.fills[i].order_id == b.fills[i].order_id);
    CHECK(a.fills[i].price == b.fills[i].price);
    CHECK(a.fills[i].qty == b.fills[i].qty);
    CHECK(a.fills[i].cash_after == b.fills[i].cash_after);
  }
  REQUIRE(a.equity_curve.size() == b.equity_curve.size());
  for (std::size_t i = 0; i < a.equity_curve.size(); ++i) CHECK(a.equity_curve[i].equity == b.equity_curve[i].equity);
  CHECK(a.events.size() == b.events.size());
  CHECK(a.trades.size() == b.trades.size());
  CHECK(a.final_equity == b.final_equity);
}
