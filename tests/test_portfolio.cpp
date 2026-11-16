#include <stdexcept>

#include "bt/portfolio.hpp"
#include "doctest.h"

using namespace bt;

TEST_CASE("portfolio: buy, average cost, partial sell, realised P&L (hand-computed)") {
  Portfolio pf(1, 1'000'000);  // Rs 10,000
  // Buy 100 @ Rs 10.00 (1000 paise): notional 100,000; fees 50.
  CHECK(pf.apply_fill(0, Side::Buy, 100, 1000, 50) == 0);
  CHECK(pf.cash() == 1'000'000 - 100'000 - 50);
  CHECK(pf.qty(0) == 100);
  CHECK(pf.avg_cost(0) == doctest::Approx(10.0));
  // Sell 40 @ Rs 12.00: notional 48,000; released cost = 100,000 * 40/100 = 40,000; realised = 8,000.
  CHECK(pf.apply_fill(0, Side::Sell, 40, 1200, 30) == 8000);
  CHECK(pf.cash() == 899'950 + 48'000 - 30);
  CHECK(pf.qty(0) == 60);
  CHECK(pf.position(0).cost == 60'000);
  CHECK(pf.avg_cost(0) == doctest::Approx(10.0));
  CHECK(pf.realized_gross() == 8000);
  CHECK(pf.fees_paid() == 80);
  // Unrealised at a mark of Rs 11.00: 60 * 1100 - 60,000 = 6,000.
  CHECK(pf.unrealized_gross(0, 1100) == 6000);
  // Equity identity: equity - initial == realised + unrealised - fees.
  const Paise equity = pf.cash() + pf.qty(0) * 1100;
  CHECK(equity - pf.initial_cash() == pf.realized_gross() + pf.unrealized_gross(0, 1100) - pf.fees_paid());
}

TEST_CASE("portfolio: proportional cost release rounds, and the last sale releases the exact remainder") {
  Portfolio pf(1, 10'000);
  pf.apply_fill(0, Side::Buy, 3, 100, 0);  // 3 @ 1.00 -> cost 300
  pf.apply_fill(0, Side::Buy, 4, 101, 0);  // 4 @ 1.01 -> cost 404; total 7 sh, 704
  CHECK(pf.position(0).cost == 704);
  // Sell 2 @ 1.10: released = 704 * 2 / 7 = 201.14 -> 201; realised = 220 - 201 = 19.
  CHECK(pf.apply_fill(0, Side::Sell, 2, 110, 0) == 19);
  CHECK(pf.position(0).cost == 503);
  // Sell the remaining 5 @ 1.20: releases exactly 503 (no rounding residue); realised = 600 - 503 = 97.
  CHECK(pf.apply_fill(0, Side::Sell, 5, 120, 0) == 97);
  CHECK(pf.qty(0) == 0);
  CHECK(pf.position(0).cost == 0);
  // Total realised = sells (220 + 600) - buys (300 + 404) = 116.
  CHECK(pf.realized_gross() == 116);
  CHECK(pf.cash() == 10'000 - 704 + 820);
}

TEST_CASE("portfolio: short round trip (hand-computed)") {
  Portfolio pf(1, 100'000);
  // Short 10 @ Rs 50.00 (5000 paise): proceeds 50,000; cost basis -50,000.
  pf.apply_fill(0, Side::Sell, 10, 5000, 0);
  CHECK(pf.qty(0) == -10);
  CHECK(pf.position(0).cost == -50'000);
  CHECK(pf.cash() == 150'000);
  CHECK(pf.avg_cost(0) == doctest::Approx(50.0));
  // Marked at Rs 45.00: unrealised = (-10 * 4500) - (-50,000) = +5,000.
  CHECK(pf.unrealized_gross(0, 4500) == 5000);
  // Cover 4 @ 45.00: notional 18,000; released = -50,000 * 4/10 = -20,000; realised = -18,000 + 20,000 = 2,000.
  CHECK(pf.apply_fill(0, Side::Buy, 4, 4500, 0) == 2000);
  // Cover the last 6 @ 45.00: notional 27,000; released -30,000; realised 3,000.
  CHECK(pf.apply_fill(0, Side::Buy, 6, 4500, 0) == 3000);
  CHECK(pf.qty(0) == 0);
  CHECK(pf.realized_gross() == 5000);
  CHECK(pf.cash() == 105'000);
}

TEST_CASE("portfolio: invariants throw") {
  Portfolio pf(2, 10'000);
  CHECK_THROWS_AS(pf.apply_fill(0, Side::Buy, 11, 1000, 0), std::logic_error);   // 11,000 > 10,000
  CHECK_THROWS_AS(pf.apply_fill(0, Side::Buy, 10, 1000, 1), std::logic_error);   // fees push it over
  CHECK_THROWS_AS(pf.apply_fill(0, Side::Buy, 0, 1000, 0), std::logic_error);
  pf.apply_fill(0, Side::Buy, 5, 1000, 0);
  CHECK_THROWS_AS(pf.apply_fill(0, Side::Sell, 6, 1000, 0), std::logic_error);   // crosses zero
  pf.apply_fill(1, Side::Sell, 1, 10, 0);                                         // a short is allowed at this layer
  CHECK_THROWS_AS(pf.apply_fill(1, Side::Buy, 2, 10, 0), std::logic_error);      // buying through zero
  CHECK_THROWS_AS(Portfolio(1, -1), std::logic_error);
  // A tiny sale whose fees exceed its proceeds must not drive cash negative.
  Portfolio p2(1, 100);
  p2.apply_fill(0, Side::Buy, 1, 100, 0);  // cash 0
  CHECK_THROWS_AS(p2.apply_fill(0, Side::Sell, 1, 100, 101), std::logic_error);
}
