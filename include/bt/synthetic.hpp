#pragma once
#include <cstdint>
#include <vector>

#include "bt/bar.hpp"

namespace bt {

// Seeded geometric-Brownian-motion generator for tests and benchmarks. NOT real market data.
//
// Per day (dt = 1/252):  log-return = (mu - sigma^2/2) dt + sigma sqrt(dt) z.
// Open gaps slightly from the previous close; high/low extend beyond open/close; prices are rounded
// to 0.01 (so CSV text round-trips exactly) and floored at 0.01; adj_close == close.
// Calendar = weekdays only (Mon-Fri); NSE holidays are NOT modelled.
//
// Determinism: raw bits come from std::mt19937_64 (fully specified by the C++ standard) and are turned
// into uniforms / normals by our own code (53-bit uniform + Box-Muller), because the std::*_distribution
// classes may give different results on different standard libraries. exp/log/cos come from the
// platform libm, so last-bit differences across toolchains are possible in principle.
struct SynthParams {
  std::uint64_t seed = 1;
  Date start = days_from_civil(2020, 1, 1);  // moved forward to the next weekday if it is a weekend
  int num_bars = 252;
  double start_price = 100.0;
  double annual_drift = 0.08;  // mu
  double annual_vol = 0.25;    // sigma
  std::int64_t base_volume = 1'000'000;
};

std::vector<Bar> generate_gbm(const SynthParams& p);

// n consecutive weekdays starting at (or after) `start`.
std::vector<Date> weekday_calendar(Date start, int n);

}  // namespace bt
