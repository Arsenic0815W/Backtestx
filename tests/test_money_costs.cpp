#include <fstream>
#include <sstream>
#include <string>

#include "bt/costs.hpp"
#include "bt/json_util.hpp"
#include "doctest.h"

using namespace bt;

namespace {

// Round-number rates chosen so every amount below can be done by hand.
CostConfig hand_cfg() {
  return CostConfig::from_json_string(R"({
    "brokerage": {"type": "flat", "flat_rupees": 20},
    "stt_buy_pct": 0.1, "stt_sell_pct": 0.1,
    "exchange_txn_pct": 0.003, "sebi_pct": 0.0001,
    "stamp_buy_pct": 0.015, "gst_pct": 18
  })");
}

}  // namespace

TEST_CASE("money: rounding is half away from zero and exact") {
  CHECK(mul_div_round(5, 1, 2) == 3);    // 2.5  -> 3
  CHECK(mul_div_round(-5, 1, 2) == -3);  // -2.5 -> -3
  CHECK(mul_div_round(7, 1, 2) == 4);    // 3.5  -> 4
  CHECK(mul_div_round(4, 1, 3) == 1);    // 1.33 -> 1
  CHECK(mul_div_round(5, 1, 3) == 2);    // 1.67 -> 2
  CHECK(mul_div_round(0, 5, 7) == 0);
  // 50 paise at 1 % = 0.5 paise -> 1 (tie away from zero)
  CHECK(apply_rate_ppb(50, rate_pct_to_ppb(1.0)) == 1);
  CHECK(apply_rate_ppb(-50, rate_pct_to_ppb(1.0)) == -1);
  // No overflow in the intermediate: 1e17 paise * 1e9 ppb needs > 64 bits.
  CHECK(apply_rate_ppb(100'000'000'000'000'000LL, 1'000'000'000) == 100'000'000'000'000'000LL);
}

TEST_CASE("money: rate conversions and formatting") {
  CHECK(rate_pct_to_ppb(0.1) == 1'000'000);
  CHECK(rate_pct_to_ppb(0.00307) == 30'700);
  CHECK(rate_pct_to_ppb(0.0001) == 1'000);
  CHECK(rate_pct_to_ppb(18.0) == 180'000'000);
  CHECK(rate_bps_to_ppb(10.0) == 1'000'000);  // 10 bps == 0.1 %
  CHECK(to_paise(19.99) == 1999);             // 19.99 is not exact in binary; rounding recovers the paisa
  CHECK(to_paise(0.1 + 0.2) == 30);           // 0.30000000000000004 -> 30 paise
  CHECK(to_paise(1234.5678) == 123457);       // nearest paisa
  CHECK(format_paise(123456) == "1234.56");
  CHECK(format_paise(-5) == "-0.05");
  CHECK(format_paise(0) == "0.00");
}

TEST_CASE("fees: hand-computed BUY of 100 shares at Rs 1000") {
  // notional = 100 * 1000 = Rs 1,00,000 = 10,000,000 paise
  //   brokerage  flat Rs 20                        =  2,000
  //   STT        0.1 %  * 10,000,000               = 10,000
  //   exchange   0.003 % * 10,000,000 = 300.0      =    300
  //   SEBI       0.0001 % * 10,000,000 = 10.0      =     10
  //   stamp      0.015 % * 10,000,000 = 1,500.0    =  1,500
  //   GST        18 % * (2,000 + 300 + 10 = 2,310) = 415.8 -> 416
  //   total                                        = 14,226
  const FeeBreakdown f = compute_fees(hand_cfg(), Side::Buy, 10'000'000);
  CHECK(f.brokerage == 2000);
  CHECK(f.stt == 10000);
  CHECK(f.exchange == 300);
  CHECK(f.sebi == 10);
  CHECK(f.stamp == 1500);
  CHECK(f.gst == 416);
  CHECK(f.total() == 14226);
}

TEST_CASE("fees: hand-computed SELL of 100 shares at Rs 1010 (no stamp duty)") {
  // notional = 10,100,000 paise
  //   brokerage 2,000;  STT 0.1 % = 10,100;  exchange 0.003 % = 303.0 -> 303;
  //   SEBI 0.0001 % = 10.1 -> 10;  stamp 0;
  //   GST 18 % * (2,000 + 303 + 10 = 2,313) = 416.34 -> 416
  //   total = 2,000 + 10,100 + 303 + 10 + 0 + 416 = 12,829
  const FeeBreakdown f = compute_fees(hand_cfg(), Side::Sell, 10'100'000);
  CHECK(f.brokerage == 2000);
  CHECK(f.stt == 10100);
  CHECK(f.exchange == 303);
  CHECK(f.sebi == 10);
  CHECK(f.stamp == 0);
  CHECK(f.gst == 416);
  CHECK(f.total() == 12829);
}

TEST_CASE("fees: GST is taken on the ROUNDED components") {
  // notional 9,500,000: exchange = 285.0, SEBI = 9.5 -> 10 (tie away), brokerage 2,000.
  // GST base = 2,000 + 285 + 10 = 2,295 (if SEBI were left at 9.5 it would be 2,294.5).
  // 18 % * 2,295 = 413.1 -> 413
  const FeeBreakdown f = compute_fees(hand_cfg(), Side::Buy, 9'500'000);
  CHECK(f.sebi == 10);
  CHECK(f.exchange == 285);
  CHECK(f.gst == 413);
}

TEST_CASE("fees: shipped DEFAULT rates (brokerage Rs 0), computed by hand from the config values") {
  // Defaults: brokerage Rs 0, STT 0.1 % both sides, exchange 0.00307 %, SEBI 0.0001 %, stamp 0.015 % (buy), GST 18 %.
  const CostConfig d = CostConfig::nse_delivery_defaults();
  // BUY 10,000,000 paise: brokerage 0; STT 10,000; exchange 0.00307 % = 307.0 -> 307; SEBI 10; stamp 1,500;
  //   GST 18 % * (0 + 307 + 10 = 317) = 57.06 -> 57;  total 10,000 + 307 + 10 + 1,500 + 57 = 11,874
  const FeeBreakdown b = compute_fees(d, Side::Buy, 10'000'000);
  CHECK(b.brokerage == 0);
  CHECK(b.stt == 10000);
  CHECK(b.exchange == 307);
  CHECK(b.sebi == 10);
  CHECK(b.stamp == 1500);
  CHECK(b.gst == 57);
  CHECK(b.total() == 11874);
  // SELL 10,100,000 paise: brokerage 0; STT 10,100; exchange 310.07 -> 310; SEBI 10.1 -> 10; stamp 0;
  //   GST 18 % * (0 + 310 + 10 = 320) = 57.6 -> 58;  total 10,100 + 310 + 10 + 58 = 10,478
  const FeeBreakdown s = compute_fees(d, Side::Sell, 10'100'000);
  CHECK(s.brokerage == 0);
  CHECK(s.stt == 10100);
  CHECK(s.exchange == 310);
  CHECK(s.sebi == 10);
  CHECK(s.stamp == 0);
  CHECK(s.gst == 58);
  CHECK(s.total() == 10478);
}

TEST_CASE("fees: percentage brokerage with min and max") {
  const CostConfig c = CostConfig::from_json_string(R"({
    "brokerage": {"type": "percent", "pct": 0.03, "min_rupees": 5, "max_rupees": 20},
    "stt_buy_pct": 0, "stt_sell_pct": 0, "exchange_txn_pct": 0, "sebi_pct": 0, "stamp_buy_pct": 0, "gst_pct": 0})");
  // 0.03 % of 10,000,000 = 3,000 paise (Rs 30) -> capped at Rs 20 = 2,000
  CHECK(compute_fees(c, Side::Buy, 10'000'000).brokerage == 2000);
  // 0.03 % of 5,000,000 = 1,500 (Rs 15): inside the band
  CHECK(compute_fees(c, Side::Buy, 5'000'000).brokerage == 1500);
  // 0.03 % of 100,000 = 30 paise -> raised to the Rs 5 minimum = 500
  CHECK(compute_fees(c, Side::Buy, 100'000).brokerage == 500);
}

TEST_CASE("slippage amounts (hand-computed)") {
  SlippageConfig none;
  CHECK(slippage_amount(none, 100000, 2000) == 0);
  SlippageConfig bps;
  bps.mode = SlippageConfig::Mode::FixedBps;
  bps.value = 10;  // 10 bps = 0.1 %
  CHECK(slippage_amount(bps, 100000, 0) == 100);
  bps.value = 1;  // 1 bp of 12,345 paise = 1.2345 -> 1
  CHECK(slippage_amount(bps, 12345, 0) == 1);
  SlippageConfig rng;
  rng.mode = SlippageConfig::Mode::RangePct;
  rng.value = 25;  // 25 % of a 1,000-paise range
  CHECK(slippage_amount(rng, 99999, 1000) == 250);
}

TEST_CASE("cost config JSON: empty object gives the defaults") {
  const CostConfig c = CostConfig::from_json_string("{}");
  const CostConfig d = CostConfig::nse_delivery_defaults();
  CHECK(compute_fees(c, Side::Buy, 12'345'678) == compute_fees(d, Side::Buy, 12'345'678));
  CHECK(compute_fees(c, Side::Sell, 12'345'678) == compute_fees(d, Side::Sell, 12'345'678));
}

TEST_CASE("cost config JSON: strict parsing") {
  CHECK_THROWS_AS(CostConfig::from_json_string(R"({"stt_buy": 0.1})"), ConfigError);             // unknown key
  CHECK_THROWS_AS(CostConfig::from_json_string(R"({"stt_buy_pct": "0.1"})"), ConfigError);       // wrong type
  CHECK_THROWS_AS(CostConfig::from_json_string(R"({"stt_buy_pct": true})"), ConfigError);        // bool is not a number
  CHECK_THROWS_AS(CostConfig::from_json_string(R"({"gst_pct": -1})"), ConfigError);              // negative rate
  CHECK_THROWS_AS(CostConfig::from_json_string(R"({"gst_pct": 101})"), ConfigError);             // > 100 %
  CHECK_THROWS_AS(CostConfig::from_json_string(R"({"brokerage": {"type": "tiered"}})"), ConfigError);
  CHECK_THROWS_AS(CostConfig::from_json_string(R"({"brokerage": {"flat": 20}})"), ConfigError);  // unknown nested key
  CHECK_THROWS_AS(CostConfig::from_json_string(R"({"brokerage": {"type": "percent", "min_rupees": 9, "max_rupees": 5}})"),
                  ConfigError);
  CHECK_THROWS_AS(CostConfig::from_json_string(R"({"meta": {"sources": [1]}})"), ConfigError);
  CHECK_THROWS_AS(CostConfig::from_json_string(R"({"meta": {"author": "x"}})"), ConfigError);
  CHECK_THROWS_AS(CostConfig::from_json_string("[1,2]"), ConfigError);
  CHECK_THROWS_AS(CostConfig::from_json_string("{not json"), ConfigError);
  // The error names the offending key.
  try {
    CostConfig::from_json_string(R"({"brokerage": {"flat_rupees": "x"}})");
    FAIL("should have thrown");
  } catch (const ConfigError& e) {
    CHECK(std::string(e.what()).find("costs.brokerage.flat_rupees") != std::string::npos);
  }
}

TEST_CASE("slippage config JSON: strict parsing") {
  const auto ok = SlippageConfig::from_json(nlohmann::json::parse(R"({"mode":"fixed_bps","value":5})"));
  CHECK(ok.mode == SlippageConfig::Mode::FixedBps);
  CHECK(ok.value == 5.0);
  CHECK(SlippageConfig::from_json(nlohmann::json::parse("{}")).mode == SlippageConfig::Mode::None);
  CHECK_THROWS_AS(SlippageConfig::from_json(nlohmann::json::parse(R"({"mode":"bps"})")), ConfigError);
  CHECK_THROWS_AS(SlippageConfig::from_json(nlohmann::json::parse(R"({"mode":"fixed_bps","value":-1})")), ConfigError);
  CHECK_THROWS_AS(SlippageConfig::from_json(nlohmann::json::parse(R"({"mode":"range_pct","value":101})")), ConfigError);
  CHECK_THROWS_AS(SlippageConfig::from_json(nlohmann::json::parse(R"({"mode":"none","extra":1})")), ConfigError);
}

namespace {
CostConfig load_shipped(const char* name) {
  std::ifstream in(std::string(BT_CONFIG_DIR) + "/" + name, std::ios::binary);
  REQUIRE(in.good());
  std::stringstream ss;
  ss << in.rdbuf();
  return CostConfig::from_json_string(ss.str());
}
}  // namespace

TEST_CASE("fees: configs/costs_nse_delivery_flat20.json pins the Rs 20 hand-computed numbers") {
  // Same rates as the default but with Rs 20 flat brokerage, so the original hand calculation applies:
  // BUY 10,000,000 paise: brokerage 2,000; STT 10,000; exchange 307; SEBI 10; stamp 1,500;
  //   GST 18 % * (2,000 + 307 + 10 = 2,317) = 417.06 -> 417;  total 14,234
  // SELL 10,100,000 paise: brokerage 2,000; STT 10,100; exchange 310; SEBI 10; stamp 0;
  //   GST 18 % * (2,000 + 310 + 10 = 2,320) = 417.6 -> 418;  total 12,838
  const CostConfig f20 = load_shipped("costs_nse_delivery_flat20.json");
  const FeeBreakdown b = compute_fees(f20, Side::Buy, 10'000'000);
  CHECK(b.brokerage == 2000);
  CHECK(b.gst == 417);
  CHECK(b.total() == 14234);
  const FeeBreakdown s = compute_fees(f20, Side::Sell, 10'100'000);
  CHECK(s.brokerage == 2000);
  CHECK(s.gst == 418);
  CHECK(s.total() == 12838);
  // It differs from the default ONLY in brokerage.
  const CostConfig d = CostConfig::nse_delivery_defaults();
  CHECK(f20.brokerage_flat_rupees == 20.0);
  CHECK(d.brokerage_flat_rupees == 0.0);
  CHECK(f20.stt_buy_pct == d.stt_buy_pct);
  CHECK(f20.stt_sell_pct == d.stt_sell_pct);
  CHECK(f20.exchange_txn_pct == d.exchange_txn_pct);
  CHECK(f20.sebi_pct == d.sebi_pct);
  CHECK(f20.stamp_buy_pct == d.stamp_buy_pct);
  CHECK(f20.gst_pct == d.gst_pct);
}

TEST_CASE("shipped configs/costs_nse_delivery.json parses and equals the built-in defaults") {
  const CostConfig f = load_shipped("costs_nse_delivery.json");
  const CostConfig d = CostConfig::nse_delivery_defaults();
  CHECK(f.brokerage_type == d.brokerage_type);
  CHECK(f.brokerage_flat_rupees == d.brokerage_flat_rupees);
  CHECK(f.brokerage_pct == d.brokerage_pct);
  CHECK(f.stt_buy_pct == d.stt_buy_pct);
  CHECK(f.stt_sell_pct == d.stt_sell_pct);
  CHECK(f.exchange_txn_pct == d.exchange_txn_pct);
  CHECK(f.sebi_pct == d.sebi_pct);
  CHECK(f.stamp_buy_pct == d.stamp_buy_pct);
  CHECK(f.gst_pct == d.gst_pct);
}
