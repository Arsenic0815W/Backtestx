#pragma once
#include <cmath>
#include <cstdint>
#include <string>

namespace bt {

// All ledger money (cash, notional, fees, P&L) is an exact int64 count of PAISE (1 rupee = 100 paise).
// int64 holds +-9.2e18 paise = +-9.2e16 rupees, far beyond any portfolio here.
// Prices from data files are doubles; they are rounded ONCE, to the nearest paisa, when they enter the
// engine (to_paise). From then on everything is integer arithmetic, so there is no drift.
using Paise = std::int64_t;

inline Paise to_paise(double rupees) { return static_cast<Paise>(std::llround(rupees * 100.0)); }
inline double to_rupees(Paise p) { return static_cast<double>(p) / 100.0; }

// A rate is held as an integer count of parts-per-billion (ppb) so that fee maths is exact:
//   1 % = 10,000,000 ppb      1 basis point = 100,000 ppb
// Resolution is 1e-9 of the base, i.e. rates finer than 0.0000001 % are rounded.
std::int64_t rate_pct_to_ppb(double pct);
std::int64_t rate_bps_to_ppb(double bps);

// round(value * num / den), ties away from zero, exact (128-bit intermediate). den must be > 0.
Paise mul_div_round(Paise value, std::int64_t num, std::int64_t den);

// round(base * ppb / 1e9), ties away from zero. THE rounding rule for every rate-based amount.
inline Paise apply_rate_ppb(Paise base, std::int64_t ppb) { return mul_div_round(base, ppb, 1'000'000'000); }

// "-1234.50" (plain, no currency symbol or grouping).
std::string format_paise(Paise p);

}  // namespace bt
