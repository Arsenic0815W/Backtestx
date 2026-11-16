#include "bt/sizing.hpp"
#include "doctest.h"
#include "test_util.hpp"

using namespace tu;

TEST_CASE("sizing: pure helpers round DOWN to whole shares") {
  CHECK(qty_fixed(5) == 5);
  CHECK(qty_fixed(-3) == 0);
  CHECK(qty_for_amount(10000, 100) == 100);
  CHECK(qty_for_amount(10050, 100) == 100);   // 100.5 -> 100
  CHECK(qty_for_amount(99.99, 100) == 0);
  CHECK(qty_for_amount(1'000'000, 333.33) == 3000);  // 3000.03 -> 3000
  CHECK(qty_for_amount(0.3, 0.1) == 3);       // 0.3 / 0.1 = 2.9999999999999996 in floating point
  CHECK(qty_for_amount(0, 100) == 0);
  CHECK(qty_for_amount(-5, 100) == 0);
  CHECK(qty_for_amount(100, 0) == 0);
  // 10 % of Rs 10,00,000 = Rs 1,00,000; at Rs 250 that is 400 shares.
  CHECK(qty_for_equity_pct(1'000'000, 10, 250) == 400);
  CHECK(qty_for_equity_pct(1'000'000, 0, 250) == 0);
}

namespace {
AlignedData two_flat_symbols(int n = 5) {
  std::vector<Bar> a, b;
  for (int i = 0; i < n; ++i) {
    a.push_back(mk(100 + i, 100, 101, 99, 100));
    b.push_back(mk(100 + i, 200, 201, 199, 200));
  }
  return align_calendar({{"A", a}, {"B", b}});
}
}  // namespace

TEST_CASE("rebalance: equal weight from cash buys round-down quantities in ascending symbol order") {
  // Equity Rs 1,00,000. Targets: A 50,000 / 100 = 500 sh; B 50,000 / 200 = 250 sh.
  const AlignedData data = two_flat_symbols();
  std::vector<OrderId> ids;
  Scripted s([&](Context& c) {
    if (c.day_index() == 0) {
      CHECK(c.equity() == doctest::Approx(100'000.0));
      ids = rebalance_equal_weight(c, {0, 1});
    }
  });
  const EngineResult r = Engine(data, engine_cfg(10'000'000, free_costs())).run(s);
  CHECK(ids == std::vector<OrderId>{1, 2});
  REQUIRE(r.fills.size() == 2);
  CHECK(r.fills[0].symbol == 0);
  CHECK(r.fills[0].qty == 500);
  CHECK(r.fills[1].symbol == 1);
  CHECK(r.fills[1].qty == 250);
  CHECK(r.final_cash == 0);
}

TEST_CASE("rebalance: sells are submitted before buys, so sale proceeds fund the purchase") {
  // Day 0: put ALL cash into A (1000 sh @ Rs 100). Day 1: rotate fully into B (Rs 200).
  // Cash is 0 when the orders reach the market, so the buy of B only succeeds because A is sold first.
  const AlignedData data = two_flat_symbols();
  std::vector<OrderId> ids;
  Scripted s([&](Context& c) {
    if (c.day_index() == 0) c.buy(0, 1000);
    if (c.day_index() == 1) ids = rebalance_to_weights(c, {{1, 1.0}});  // A is held but not listed -> sold
  });
  const EngineResult r = Engine(data, engine_cfg(10'000'000, free_costs())).run(s);
  REQUIRE(ids.size() == 2);
  // ids: buy A was id 1; then rebalance: sell A (id 2) then buy B (id 3)
  CHECK(ids[0] == 2);
  CHECK(ids[1] == 3);
  REQUIRE(r.fills.size() == 3);
  CHECK(r.fills[1].side == Side::Sell);
  CHECK(r.fills[1].symbol == 0);
  CHECK(r.fills[1].qty == 1000);
  CHECK(r.fills[2].side == Side::Buy);
  CHECK(r.fills[2].symbol == 1);
  CHECK(r.fills[2].qty == 500);  // 1,00,000 / 200
  CHECK(r.events.empty());
  CHECK(r.final_cash == 0);
  REQUIRE(r.open_positions.size() == 1);
  CHECK(r.open_positions[0].symbol == 1);
}

TEST_CASE("rebalance: cash buffer leaves headroom; unlisted symbols and empty history are handled") {
  const AlignedData data = two_flat_symbols();
  Scripted s([&](Context& c) {
    if (c.day_index() == 0) rebalance_to_weights(c, {{0, 1.0}}, 10.0);  // 90 % of 1,00,000 / 100 = 900
  });
  const EngineResult r = Engine(data, engine_cfg(10'000'000, free_costs())).run(s);
  REQUIRE(r.fills.size() == 1);
  CHECK(r.fills[0].qty == 900);

  // Late-listed symbol: no visible bar yet -> no order, no throw.
  const AlignedData late = align_calendar({{"A", {mk(100, 100, 101, 99, 100), mk(101, 100, 101, 99, 100)}},
                                           {"L", {mk(101, 50, 51, 49, 50)}}});
  std::vector<OrderId> ids;
  Scripted s2([&](Context& c) {
    if (c.day_index() == 0) ids = rebalance_equal_weight(c, {0, 1});
  });
  Engine(late, engine_cfg(10'000'000, free_costs())).run(s2);
  CHECK(ids.size() == 1);  // only A (500 sh); L is skipped

  Scripted s3([&](Context& c) { CHECK_THROWS_AS(rebalance_to_weights(c, {{9, 1.0}}), std::invalid_argument); });
  Engine(data, engine_cfg(10'000'000, free_costs())).run(s3);
}
