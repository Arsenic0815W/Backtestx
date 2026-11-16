#include <cmath>

#include "bt/adjust.hpp"
#include "bt/csv.hpp"
#include "doctest.h"

using namespace bt;

// A hand-made 2-for-1 split where the raw (unadjusted) Close halves overnight but adj_close
// is already back-adjusted. Factor f = adj_close / close.
//
//   day1: C=200, adj=100  -> f = 0.5   -> O 198*0.5=99,  H 204*0.5=102, L 196*0.5=98,  C' = 100
//   day2: C=206, adj=103  -> f = 0.5   -> O 200*0.5=100, H 210*0.5=105, L 198*0.5=99,  C' = 103
//   day3: C=105, adj=105  -> f = 1.0   -> unchanged (post-split prices are already on the new basis)
// 0.5 and these products are exact in binary floating point, so exact == comparisons are legitimate.
static std::vector<Bar> split_bars() {
  return {
      {100, 198, 204, 196, 200, 100, 10},
      {101, 200, 210, 198, 206, 103, 20},
      {102, 104, 106, 102, 105, 105, 40},
  };
}

TEST_CASE("adjust: hand-computed 2:1 split") {
  const auto adj = back_adjust(split_bars());
  REQUIRE(adj.size() == 3);

  CHECK(adj[0].open == 99.0);
  CHECK(adj[0].high == 102.0);
  CHECK(adj[0].low == 98.0);
  CHECK(adj[0].close == 100.0);
  CHECK(adj[0].adj_close == 100.0);

  CHECK(adj[1].open == 100.0);
  CHECK(adj[1].high == 105.0);
  CHECK(adj[1].low == 99.0);
  CHECK(adj[1].close == 103.0);

  CHECK(adj[2] == split_bars()[2]);

  // Volume untouched; dates untouched.
  CHECK(adj[0].volume == 10);
  CHECK(adj[1].volume == 20);
  CHECK(adj[2].volume == 40);
  CHECK(adj[1].date == 101);
}

TEST_CASE("adjust: removes the fake crash that the raw series shows") {
  const auto raw = split_bars();
  const auto adj = back_adjust(raw);
  // Raw day2->day3 close-to-close: 105/206 - 1 = -49.0%  (looks like a crash).
  CHECK(raw[2].close / raw[1].close - 1.0 == doctest::Approx(-0.4903).epsilon(1e-3));
  // Adjusted: 105/103 - 1 = +1.94%.
  CHECK(adj[2].close / adj[1].close - 1.0 == doctest::Approx(2.0 / 103.0).epsilon(1e-12));
}

TEST_CASE("adjust: input is not modified and output always passes check_bar") {
  const auto raw = split_bars();
  const auto copy = raw;
  const auto adj = back_adjust(raw);
  CHECK(raw == copy);
  for (const Bar& b : adj) CHECK_FALSE(check_bar(b).has_value());
}

TEST_CASE("adjust: awkward ratio with close == high stays a valid bar (rounding guard)") {
  // f = 97.3333/100 is not exactly representable; close == high == 100 so close' = 97.3333 must not
  // end up above the scaled high.
  const std::vector<Bar> raw = {{1, 99, 100, 98, 100, 97.3333, 5}, {2, 99, 100, 98, 100, 97.3333, 5}};
  for (const Bar& b : back_adjust(raw)) {
    CHECK_FALSE(check_bar(b).has_value());
    CHECK(b.close == 97.3333);
  }
}

TEST_CASE("adjust: ratio of 1 is the identity") {
  const std::vector<Bar> raw = {{1, 100, 105, 99, 104, 104, 7}};
  CHECK(back_adjust(raw)[0] == raw[0]);
}

TEST_CASE("adjust: empty input") { CHECK(back_adjust({}).empty()); }
