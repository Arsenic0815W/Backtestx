#include "bt/fill.hpp"
#include "doctest.h"

using namespace bt;

namespace {

// Rupees in, paise inside (x100), so every expected value below is an easy-to-read integer.
BarP bar(double o, double h, double l, double c, std::int64_t vol = 1'000'000) {
  return BarP{to_paise(o), to_paise(h), to_paise(l), to_paise(c), vol};
}

Order ord(Side side, OrderType type, double price, std::int64_t qty = 10) {
  Order o;
  o.id = 1;
  o.side = side;
  o.type = type;
  o.qty = qty;
  o.remaining = qty;
  o.price = type == OrderType::Market ? 0 : to_paise(price);
  return o;
}

const SlippageConfig kNoSlip{};

SlippageConfig bps(double v) {
  SlippageConfig s;
  s.mode = SlippageConfig::Mode::FixedBps;
  s.value = v;
  return s;
}

// Returns the fill price in paise, or -1 for no fill.
Paise px(const Order& o, const BarP& b, const SlippageConfig& s = kNoSlip) {
  const auto q = try_fill(o, b, s, 0.0);
  return q ? q->price : -1;
}

}  // namespace

// Reference bar for most cases: open 1000, high 1100, low 900, close 1050.
// Gap-up bar:   open 1200, high 1250, low 1180.   Gap-down bar: open 800, high 850, low 780.

TEST_CASE("fill: market orders fill at the open") {
  const BarP b = bar(1000, 1100, 900, 1050);
  CHECK(px(ord(Side::Buy, OrderType::Market, 0), b) == 100000);
  CHECK(px(ord(Side::Sell, OrderType::Market, 0), b) == 100000);
}

TEST_CASE("fill: market order slippage is adverse and clamped to the bar's range") {
  const BarP b = bar(1000, 1100, 900, 1050);
  CHECK(px(ord(Side::Buy, OrderType::Market, 0), b, bps(10)) == 100100);  // +10 bps of 100000 = +100
  CHECK(px(ord(Side::Sell, OrderType::Market, 0), b, bps(10)) == 99900);
  // open == high: slipped buy would be 110110, clamped to the high 110000
  CHECK(px(ord(Side::Buy, OrderType::Market, 0), bar(1100, 1100, 1000, 1050), bps(10)) == 110000);
  // open == low: slipped sell would be 89910, clamped to the low 90000
  CHECK(px(ord(Side::Sell, OrderType::Market, 0), bar(900, 1000, 900, 950), bps(10)) == 90000);
  // range slippage: 25 % of (110000 - 90000) = 5000
  SlippageConfig r;
  r.mode = SlippageConfig::Mode::RangePct;
  r.value = 25;
  CHECK(px(ord(Side::Buy, OrderType::Market, 0), b, r) == 105000);
}

TEST_CASE("fill: limit buy") {
  const BarP b = bar(1000, 1100, 900, 1050);
  CHECK(px(ord(Side::Buy, OrderType::Limit, 950), b) == 95000);     // inside the range: fills at the limit
  CHECK(px(ord(Side::Buy, OrderType::Limit, 1000), b) == 100000);   // limit == open
  CHECK(px(ord(Side::Buy, OrderType::Limit, 1050), b) == 100000);   // limit above open: better price, the open
  CHECK(px(ord(Side::Buy, OrderType::Limit, 900), b) == 90000);     // touches the low exactly: fills
  CHECK(px(ord(Side::Buy, OrderType::Limit, 899.99), b) == -1);     // never reached
  CHECK(px(ord(Side::Buy, OrderType::Limit, 800), b) == -1);
  // gap DOWN through the limit: open 900 is already below the limit 1000 -> fill at the open, not the limit
  CHECK(px(ord(Side::Buy, OrderType::Limit, 1000), bar(900, 950, 880, 920)) == 90000);
  CHECK(px(ord(Side::Buy, OrderType::Limit, 890), bar(900, 950, 880, 920)) == 89000);
  // gap UP above the limit: low 1180 never reaches 1100
  CHECK(px(ord(Side::Buy, OrderType::Limit, 1100), bar(1200, 1250, 1180, 1220)) == -1);
  // limits are never slipped
  CHECK(px(ord(Side::Buy, OrderType::Limit, 950), b, bps(50)) == 95000);
}

TEST_CASE("fill: limit sell") {
  const BarP b = bar(1000, 1100, 900, 1050);
  CHECK(px(ord(Side::Sell, OrderType::Limit, 1050), b) == 105000);
  CHECK(px(ord(Side::Sell, OrderType::Limit, 1000), b) == 100000);  // limit == open
  CHECK(px(ord(Side::Sell, OrderType::Limit, 950), b) == 100000);   // limit below open: better price, the open
  CHECK(px(ord(Side::Sell, OrderType::Limit, 1100), b) == 110000);  // touches the high exactly
  CHECK(px(ord(Side::Sell, OrderType::Limit, 1100.01), b) == -1);
  // gap UP through the limit: open 1200 above limit 1100 -> fill at the open
  CHECK(px(ord(Side::Sell, OrderType::Limit, 1100), bar(1200, 1250, 1180, 1220)) == 120000);
  // gap DOWN below the limit: high 850 never reaches 900
  CHECK(px(ord(Side::Sell, OrderType::Limit, 900), bar(800, 850, 780, 820)) == -1);
  CHECK(px(ord(Side::Sell, OrderType::Limit, 1050), b, bps(50)) == 105000);  // no slippage on limits
}

TEST_CASE("fill: stop buy") {
  const BarP b = bar(1000, 1100, 900, 1050);
  CHECK(px(ord(Side::Buy, OrderType::Stop, 1050), b) == 105000);   // triggers intrabar at the stop
  CHECK(px(ord(Side::Buy, OrderType::Stop, 1100), b) == 110000);   // touches the high exactly
  CHECK(px(ord(Side::Buy, OrderType::Stop, 1100.01), b) == -1);    // never triggered
  CHECK(px(ord(Side::Buy, OrderType::Stop, 1000), b) == 100000);   // stop == open
  CHECK(px(ord(Side::Buy, OrderType::Stop, 950), b) == 100000);    // stop below the open: already triggered, open
  // gap UP through the stop: fills at the open, not at the (better) stop
  CHECK(px(ord(Side::Buy, OrderType::Stop, 1100), bar(1200, 1250, 1180, 1220)) == 120000);
  // slippage is added to a triggered stop, then clamped to the high
  CHECK(px(ord(Side::Buy, OrderType::Stop, 1050), b, bps(10)) == 105105);
  CHECK(px(ord(Side::Buy, OrderType::Stop, 1100), b, bps(10)) == 110000);  // 110110 clamped to high 110000
}

TEST_CASE("fill: stop sell") {
  const BarP b = bar(1000, 1100, 900, 1050);
  CHECK(px(ord(Side::Sell, OrderType::Stop, 950), b) == 95000);
  CHECK(px(ord(Side::Sell, OrderType::Stop, 900), b) == 90000);    // touches the low exactly
  CHECK(px(ord(Side::Sell, OrderType::Stop, 899.99), b) == -1);
  CHECK(px(ord(Side::Sell, OrderType::Stop, 1050), b) == 100000);  // stop above the open: already triggered, open
  // gap DOWN through the stop: fills at the open (worse than the stop)
  CHECK(px(ord(Side::Sell, OrderType::Stop, 900), bar(800, 850, 780, 820)) == 80000);
  CHECK(px(ord(Side::Sell, OrderType::Stop, 900), bar(800, 850, 780, 820), bps(10)) == 79920);  // 80000 - 80
  CHECK(px(ord(Side::Sell, OrderType::Stop, 900), bar(800, 850, 800, 820), bps(10)) == 80000);  // 79920 clamped to low
  // gap UP: the stop below is not hit
  CHECK(px(ord(Side::Sell, OrderType::Stop, 1000), bar(1200, 1250, 1180, 1220)) == -1);
}

TEST_CASE("fill: orders that never touch") {
  const BarP b = bar(1000, 1100, 900, 1050);
  CHECK(px(ord(Side::Buy, OrderType::Limit, 800), b) == -1);
  CHECK(px(ord(Side::Sell, OrderType::Limit, 1200), b) == -1);
  CHECK(px(ord(Side::Buy, OrderType::Stop, 1200), b) == -1);
  CHECK(px(ord(Side::Sell, OrderType::Stop, 800), b) == -1);
}

TEST_CASE("fill: volume participation cap") {
  const BarP b = bar(1000, 1100, 900, 1050, 1000);
  Order o = ord(Side::Buy, OrderType::Market, 0, 500);
  CHECK(try_fill(o, b, kNoSlip, 0.0)->qty == 500);   // off
  CHECK(try_fill(o, b, kNoSlip, 0.1)->qty == 100);   // 10 % of 1000
  o.remaining = 50;
  CHECK(try_fill(o, b, kNoSlip, 0.1)->qty == 50);    // cap above what is left
  o.remaining = 500;
  CHECK_FALSE(try_fill(o, bar(1000, 1100, 900, 1050, 5), kNoSlip, 0.1).has_value());  // floor(0.5) = 0 shares
  CHECK_FALSE(try_fill(o, bar(1000, 1100, 900, 1050, 0), kNoSlip, 0.1).has_value());  // zero volume
  CHECK(try_fill(o, bar(1000, 1100, 900, 1050, 0), kNoSlip, 0.0).has_value());        // ...but fine with the cap off
  // remaining (not qty) is what is filled
  o.qty = 500;
  o.remaining = 120;
  CHECK(try_fill(o, bar(1000, 1100, 900, 1050, 100000), kNoSlip, 0.0)->qty == 120);
}
