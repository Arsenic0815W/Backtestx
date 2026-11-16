#include <cmath>

#include "bt/csv.hpp"
#include "bt/synthetic.hpp"
#include "doctest.h"

using namespace bt;

TEST_CASE("synthetic: same seed -> identical bars; different seed -> different bars") {
  SynthParams p;
  p.seed = 7;
  p.num_bars = 300;
  const auto a = generate_gbm(p);
  const auto b = generate_gbm(p);
  CHECK(a == b);
  p.seed = 8;
  CHECK(generate_gbm(p) != a);
}

TEST_CASE("synthetic: 20 years of bars are all valid, positive, weekday-only and strictly increasing") {
  SynthParams p;
  p.seed = 99;
  p.num_bars = 5000;
  const auto bars = generate_gbm(p);
  REQUIRE(bars.size() == 5000);
  for (std::size_t i = 0; i < bars.size(); ++i) {
    REQUIRE_FALSE(check_bar(bars[i]).has_value());
    REQUIRE(bars[i].volume >= 1);
    REQUIRE_FALSE(is_weekend(bars[i].date));
    if (i > 0) REQUIRE(bars[i].date > bars[i - 1].date);
  }
}

TEST_CASE("synthetic: a weekend start moves to Monday") {
  SynthParams p;
  p.start = parse_date("2024-01-06").value();  // Saturday
  p.num_bars = 3;
  const auto bars = generate_gbm(p);
  CHECK(format_date(bars[0].date) == "2024-01-08");
  CHECK(format_date(bars[1].date) == "2024-01-09");
  const auto cal = weekday_calendar(parse_date("2024-01-12").value(), 3);  // Fri, Mon, Tue
  CHECK(format_date(cal[0]) == "2024-01-12");
  CHECK(format_date(cal[1]) == "2024-01-15");
  CHECK(format_date(cal[2]) == "2024-01-16");
}

TEST_CASE("synthetic: CSV written by write_csv loads back bit-for-bit identical") {
  SynthParams p;
  p.seed = 3;
  p.num_bars = 1000;
  const auto bars = generate_gbm(p);
  const auto r = load_csv_string(to_csv_string(bars));
  REQUIRE(r.ok());
  CHECK(r.bars == bars);
}

TEST_CASE("synthetic: daily log-return volatility matches the requested annual vol") {
  SynthParams p;
  p.seed = 5;
  p.num_bars = 50000;
  p.annual_vol = 0.25;
  const auto bars = generate_gbm(p);
  double sum = 0, sum2 = 0;
  const std::size_t n = bars.size() - 1;
  for (std::size_t i = 1; i < bars.size(); ++i) {
    const double r = std::log(bars[i].close / bars[i - 1].close);
    sum += r;
    sum2 += r * r;
  }
  const double mean = sum / static_cast<double>(n);
  const double sd = std::sqrt(sum2 / static_cast<double>(n) - mean * mean);
  const double expected = 0.25 / std::sqrt(252.0);
  // Standard error of a sample sd at n=50000 is ~0.3% of sd; 3% is a very loose band.
  CHECK(sd == doctest::Approx(expected).epsilon(0.03));
}

TEST_CASE("synthetic: regression snapshot for seed 42 (guards accidental changes to the generator)") {
  SynthParams p;
  p.seed = 42;
  p.num_bars = 5;
  const auto bars = generate_gbm(p);
  REQUIRE(bars.size() == 5);
  // These numbers were recorded from this generator's own output (not independently derived): they
  // detect an accidental change to the algorithm or RNG stream, not a mathematical error.
  const double close[5] = {99.26, 94.8, 95.49, 94.99, 93.79};
  const std::int64_t volume[5] = {688980, 1977968, 1167161, 1589831, 792006};
  for (int i = 0; i < 5; ++i) {
    CAPTURE(i);
    CHECK(bars[static_cast<std::size_t>(i)].close == close[i]);
    CHECK(bars[static_cast<std::size_t>(i)].volume == volume[i]);
  }
  CHECK(format_date(bars[0].date) == "2020-01-01");
  CHECK(format_date(bars[3].date) == "2020-01-06");  // skips the weekend
  CHECK(bars[0].open == 100.16);
  CHECK(bars[0].high == 100.45);
  CHECK(bars[0].low == 98.69);
}
