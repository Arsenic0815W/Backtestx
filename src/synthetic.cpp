#include "bt/synthetic.hpp"

#include <algorithm>
#include <cmath>
#include <random>

namespace bt {
namespace {

class Rng {
 public:
  explicit Rng(std::uint64_t seed) : eng_(seed) {}

  // Uniform in (0, 1): top 53 bits plus half an ulp, so 0 is never returned (log(0) in Box-Muller).
  double uniform() { return (static_cast<double>(eng_() >> 11) + 0.5) * (1.0 / 9007199254740992.0); }

  // Box-Muller; the sine half is discarded to keep the stream trivially reproducible.
  double normal() {
    const double u1 = uniform();
    const double u2 = uniform();
    return std::sqrt(-2.0 * std::log(u1)) * std::cos(6.283185307179586 * u2);
  }

 private:
  std::mt19937_64 eng_;
};

double to_cents(double x) { return std::max(0.01, std::round(x * 100.0) / 100.0); }

}  // namespace

std::vector<Date> weekday_calendar(Date start, int n) {
  std::vector<Date> out;
  out.reserve(static_cast<std::size_t>(std::max(n, 0)));
  Date d = start;
  while (static_cast<int>(out.size()) < n) {
    if (!is_weekend(d)) out.push_back(d);
    ++d;
  }
  return out;
}

std::vector<Bar> generate_gbm(const SynthParams& p) {
  const std::vector<Date> days = weekday_calendar(p.start, p.num_bars);
  std::vector<Bar> bars;
  bars.reserve(days.size());

  Rng rng(p.seed);
  const double dt = 1.0 / 252.0;
  const double sigma_d = p.annual_vol * std::sqrt(dt);
  const double drift_d = (p.annual_drift - 0.5 * p.annual_vol * p.annual_vol) * dt;

  double prev = to_cents(p.start_price);
  for (Date d : days) {
    const double z_ret = rng.normal();
    const double z_gap = rng.normal();
    const double z_hi = rng.normal();
    const double z_lo = rng.normal();
    const double z_vol = rng.normal();

    const double open_raw = prev * std::exp(0.2 * sigma_d * z_gap);
    const double close_raw = prev * std::exp(drift_d + sigma_d * z_ret);
    const double hi_raw = std::max(open_raw, close_raw) * std::exp(0.5 * sigma_d * std::fabs(z_hi));
    const double lo_raw = std::min(open_raw, close_raw) * std::exp(-0.5 * sigma_d * std::fabs(z_lo));

    Bar b;
    b.date = d;
    b.open = to_cents(open_raw);
    b.close = to_cents(close_raw);
    b.high = std::max({to_cents(hi_raw), b.open, b.close});
    b.low = std::min({to_cents(lo_raw), b.open, b.close});
    b.adj_close = b.close;
    b.volume = std::max<std::int64_t>(
        1, static_cast<std::int64_t>(std::llround(static_cast<double>(p.base_volume) * std::exp(0.3 * z_vol))));
    bars.push_back(b);
    prev = b.close;
  }
  return bars;
}

}  // namespace bt
