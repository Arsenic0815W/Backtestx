#include "bt/money.hpp"

#include <cstdio>

namespace bt {

std::int64_t rate_pct_to_ppb(double pct) { return static_cast<std::int64_t>(std::llround(pct * 1e7)); }
std::int64_t rate_bps_to_ppb(double bps) { return static_cast<std::int64_t>(std::llround(bps * 1e5)); }

Paise mul_div_round(Paise value, std::int64_t num, std::int64_t den) {
  __int128 n = static_cast<__int128>(value) * num;
  const bool neg = n < 0;
  if (neg) n = -n;
  // floor((2n + den) / (2 den)) == round-half-up of n/den for n >= 0
  const __int128 q = (2 * n + den) / (2 * static_cast<__int128>(den));
  return static_cast<Paise>(neg ? -q : q);
}

std::string format_paise(Paise p) {
  const bool neg = p < 0;
  const unsigned long long a = neg ? 0ULL - static_cast<unsigned long long>(p) : static_cast<unsigned long long>(p);
  char buf[48];
  std::snprintf(buf, sizeof buf, "%s%llu.%02llu", neg ? "-" : "", a / 100, a % 100);
  return buf;
}

}  // namespace bt
