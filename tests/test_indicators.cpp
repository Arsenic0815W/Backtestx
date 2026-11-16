// Differential tests: every incremental indicator is compared with a NAIVE O(n)-per-point reference that is
// written from the textual definitions in indicators.hpp (not by transcribing the incremental code), on random
// data, edge cases and long series.
#include <algorithm>
#include <cmath>
#include <random>
#include <vector>

#include "bt/indicators.hpp"
#include "doctest.h"

using namespace bt;

namespace {

struct Rnd {
  std::mt19937_64 g;
  explicit Rnd(std::uint64_t seed) : g(seed) {}
  double u() { return static_cast<double>(g() >> 11) / 9007199254740992.0; }
};

std::vector<double> walk(std::size_t n, std::uint64_t seed, double start = 100.0, double step = 2.0) {
  Rnd r(seed);
  std::vector<double> x;
  double v = start;
  for (std::size_t i = 0; i < n; ++i) {
    v = std::max(1.0, v + (r.u() - 0.5) * step);
    x.push_back(v);
  }
  return x;
}

struct Ohlc { std::vector<double> h, l, c; };
// Bars with real gaps: each bar opens up to +-4 away from the previous close, so about half the bars lie ENTIRELY
// above or below the previous close (that is when |high-prev_close| or |low-prev_close| is the true range).
Ohlc make_ohlc(std::size_t n, std::uint64_t seed) {
  Rnd r(seed);
  Ohlc o;
  double prev = 100.0;
  for (std::size_t i = 0; i < n; ++i) {
    const double open = std::max(10.0, prev + (r.u() - 0.5) * 8.0);
    const double close = std::max(10.0, open + (r.u() - 0.5) * 3.0);
    o.h.push_back(std::max(open, close) + r.u() * 1.0);
    o.l.push_back(std::min(open, close) - r.u() * 1.0);
    o.c.push_back(close);
    prev = close;
  }
  return o;
}

double rel(double a, double b) { return std::fabs(a - b) / std::max(std::fabs(b), 1e-12); }

// ---------------- naive references (from the written definitions) ----------------
double sma_ref(const std::vector<double>& x, std::size_t i, std::size_t n) {
  double s = 0;
  for (std::size_t j = i + 1 - n; j <= i; ++j) s += x[j];
  return s / static_cast<double>(n);
}

double ema_ref(const std::vector<double>& x, std::size_t i, std::size_t n) {
  const double a = 2.0 / (static_cast<double>(n) + 1.0);
  double e = sma_ref(x, n - 1, n);  // seed = SMA of the first n inputs
  for (std::size_t j = n; j <= i; ++j) e = a * x[j] + (1.0 - a) * e;
  return e;
}

double rsi_ref(const std::vector<double>& x, std::size_t i, std::size_t n) {
  // changes d_j = x_j - x_{j-1}, j = 1..i
  auto gain = [&](std::size_t j) { return std::max(x[j] - x[j - 1], 0.0); };
  auto loss = [&](std::size_t j) { return std::max(x[j - 1] - x[j], 0.0); };
  double ag = 0, al = 0;
  for (std::size_t j = 1; j <= n; ++j) { ag += gain(j); al += loss(j); }
  ag /= static_cast<double>(n);
  al /= static_cast<double>(n);
  for (std::size_t j = n + 1; j <= i; ++j) {
    ag = (ag * static_cast<double>(n - 1) + gain(j)) / static_cast<double>(n);
    al = (al * static_cast<double>(n - 1) + loss(j)) / static_cast<double>(n);
  }
  if (al == 0.0) return ag == 0.0 ? 50.0 : 100.0;
  return 100.0 - 100.0 / (1.0 + ag / al);
}

double atr_ref(const Ohlc& o, std::size_t i, std::size_t n) {
  auto tr = [&](std::size_t j) {
    double t = o.h[j] - o.l[j];
    if (j > 0) t = std::max({t, std::fabs(o.h[j] - o.c[j - 1]), std::fabs(o.l[j] - o.c[j - 1])});
    return t;
  };
  double a = 0;
  for (std::size_t j = 0; j < n; ++j) a += tr(j);
  a /= static_cast<double>(n);
  for (std::size_t j = n; j <= i; ++j) a = (a * static_cast<double>(n - 1) + tr(j)) / static_cast<double>(n);
  return a;
}

double max_ref(const std::vector<double>& x, std::size_t i, std::size_t n) {
  return *std::max_element(x.begin() + static_cast<std::ptrdiff_t>(i + 1 - n), x.begin() + static_cast<std::ptrdiff_t>(i + 1));
}
double min_ref(const std::vector<double>& x, std::size_t i, std::size_t n) {
  return *std::min_element(x.begin() + static_cast<std::ptrdiff_t>(i + 1 - n), x.begin() + static_cast<std::ptrdiff_t>(i + 1));
}

double pop_sd_ref(const std::vector<double>& x, std::size_t i, std::size_t n) {
  const double m = sma_ref(x, i, n);
  double v = 0;
  for (std::size_t j = i + 1 - n; j <= i; ++j) v += (x[j] - m) * (x[j] - m);
  return std::sqrt(v / static_cast<double>(n));
}

const std::size_t kPeriods[] = {1, 2, 3, 5, 14, 20, 50};

}  // namespace

// =============== hand-checked values (arithmetic done on paper, independent of the code) ===============

TEST_CASE("indicators: hand-checked small examples") {
  {  // SMA(3) of 1..5 -> 2, 3, 4
    Sma s(3);
    std::vector<double> out;
    for (double v : {1, 2, 3, 4, 5}) { s.update(v); if (s.ready()) out.push_back(s.value()); }
    CHECK(out == std::vector<double>{2, 3, 4});
  }
  {  // EMA(3): alpha 0.5, seed (1+2+3)/3 = 2, then 0.5*4+0.5*2 = 3, then 0.5*5+0.5*3 = 4
    Ema e(3);
    std::vector<double> out;
    for (double v : {1, 2, 3, 4, 5}) { e.update(v); if (e.ready()) out.push_back(e.value()); }
    CHECK(out == std::vector<double>{2, 3, 4});
  }
  {  // RSI(2) on 10 9 8 9 10 11 12 11 10 9 (|change| is always 1, so RSI = 100 * avg_gain)
    // changes -1 -1 +1 +1 +1 +1 -1 -1 -1;  first avg over 2 changes: gain 0, loss 1 -> RSI 0
    // then gain 0.5 -> 50; 0.75 -> 75; 0.875 -> 87.5; 0.9375 -> 93.75; 0.46875 -> 46.875; 0.234375 -> 23.4375; 0.1171875
    Rsi r(2);
    std::vector<double> out;
    for (double v : {10, 9, 8, 9, 10, 11, 12, 11, 10, 9}) { r.update(v); if (r.ready()) out.push_back(r.value()); }
    REQUIRE(out.size() == 8);
    const double expect[] = {0, 50, 75, 87.5, 93.75, 46.875, 23.4375, 11.71875};
    for (std::size_t i = 0; i < 8; ++i) CHECK(out[i] == doctest::Approx(expect[i]).epsilon(1e-12));
  }
  {  // ATR(2): bars (h,l,c): (11,9,10) (12,10,11) (12,10,11) (12,10,11) (14,11,13)
    // TR = 2 (first bar: h-l), 2, 2, 2, 3.  Seed = mean(2,2) = 2, then 2, 2, (2*1+3)/2 = 2.5
    Atr a(2);
    const double H[] = {11, 12, 12, 12, 14}, L[] = {9, 10, 10, 10, 11}, C[] = {10, 11, 11, 11, 13};
    std::vector<double> out;
    for (int i = 0; i < 5; ++i) { a.update(H[i], L[i], C[i]); if (a.ready()) out.push_back(a.value()); }
    CHECK(out == std::vector<double>{2, 2, 2, 2.5});
  }
  {  // ATR(2) with gaps. Bars (h,l,c): (10,8,9)  then a gap DOWN (7,5,6)  then a gap UP (14,12,13)
    // TR1 = 10-8 = 2.   TR2 = max(7-5=2, |7-9|=2, |5-9|=4) = 4 (low - prev close dominates).
    // TR3 = max(14-12=2, |14-6|=8, |12-6|=6) = 8 (high - prev close dominates).
    // ATR(2): seed (2+4)/2 = 3 at bar 2, then (3*1 + 8)/2 = 5.5 at bar 3.
    Atr a(2);
    a.update(10, 8, 9);
    a.update(7, 5, 6);
    REQUIRE(a.ready());
    CHECK(a.value() == 3.0);
    a.update(14, 12, 13);
    CHECK(a.value() == 5.5);
  }
  {  // rolling max / min, window 3, on 1 3 2 5 4 4 1
    RollingMax mx(3);
    RollingMin mn(3);
    std::vector<double> a, b;
    for (double v : {1, 3, 2, 5, 4, 4, 1}) { mx.update(v); mn.update(v); if (mx.ready()) { a.push_back(mx.value()); b.push_back(mn.value()); } }
    CHECK(a == std::vector<double>{3, 5, 5, 5, 4});
    CHECK(b == std::vector<double>{1, 2, 2, 4, 1});
  }
  {  // Bollinger(3, 2) on 1 2 3: mean 2, population variance (1+0+1)/3 = 2/3
    Bollinger bb(3, 2.0);
    for (double v : {1, 2, 3}) bb.update(v);
    REQUIRE(bb.ready());
    CHECK(bb.middle() == doctest::Approx(2.0));
    CHECK(bb.stddev() == doctest::Approx(std::sqrt(2.0 / 3.0)));
    CHECK(bb.upper() == doctest::Approx(2.0 + 2.0 * std::sqrt(2.0 / 3.0)));
    CHECK(bb.lower() == doctest::Approx(2.0 - 2.0 * std::sqrt(2.0 / 3.0)));
  }
}

TEST_CASE("indicators: ready() timing and NaN before it") {
  for (std::size_t n : {1u, 2u, 5u, 14u}) {
    CAPTURE(n);
    Sma s(n); Ema e(n); RollingMax mx(n); RollingMin mn(n); Bollinger b(n, 2.0); Rsi r(n); Atr a(n);
    for (std::size_t i = 0; i < n + 3; ++i) {
      const double v = 100.0 + static_cast<double>(i);
      s.update(v); e.update(v); mx.update(v); mn.update(v); b.update(v); r.update(v); a.update(v + 1, v - 1, v);
      const bool after_n = i + 1 >= n;       // n inputs seen
      const bool after_n1 = i + 1 >= n + 1;  // n+1 inputs seen (RSI needs n changes)
      CHECK(s.ready() == after_n);
      CHECK(e.ready() == after_n);
      CHECK(mx.ready() == after_n);
      CHECK(mn.ready() == after_n);
      CHECK(b.ready() == after_n);
      CHECK(a.ready() == after_n);
      CHECK(r.ready() == after_n1);
      if (!after_n) {
        CHECK(std::isnan(s.value()));
        CHECK(std::isnan(e.value()));
        CHECK(std::isnan(mx.value()));
        CHECK(std::isnan(b.upper()));
        CHECK(std::isnan(a.value()));
      }
      if (!after_n1) CHECK(std::isnan(r.value()));
    }
  }
}

TEST_CASE("indicators: invalid parameters throw") {
  CHECK_THROWS_AS(Sma(0), std::invalid_argument);
  CHECK_THROWS_AS(Ema(0), std::invalid_argument);
  CHECK_THROWS_AS(Rsi(0), std::invalid_argument);
  CHECK_THROWS_AS(Atr(0), std::invalid_argument);
  CHECK_THROWS_AS(RollingMax(0), std::invalid_argument);
  CHECK_THROWS_AS(RollingMin(0), std::invalid_argument);
  CHECK_THROWS_AS(Bollinger(0, 2.0), std::invalid_argument);
  CHECK_THROWS_AS(Bollinger(5, -1.0), std::invalid_argument);
}

// =============== differential tests against naive references ===============

TEST_CASE("differential: SMA, EMA, Bollinger vs naive on random walks") {
  for (std::size_t n : kPeriods) {
    CAPTURE(n);
    const auto x = walk(3000, 100 + n);
    Sma sma(n); Ema ema(n); Bollinger bb(n, 2.0);
    double worst = 0;
    for (std::size_t i = 0; i < x.size(); ++i) {
      sma.update(x[i]); ema.update(x[i]); bb.update(x[i]);
      if (i + 1 < n) continue;
      worst = std::max({worst, rel(sma.value(), sma_ref(x, i, n)), rel(ema.value(), ema_ref(x, i, n)),
                        rel(bb.middle(), sma_ref(x, i, n))});
      // Absolute check for the sd: it may legitimately be exactly 0 (n = 1).
      REQUIRE(std::fabs(bb.stddev() - pop_sd_ref(x, i, n)) < 1e-9);
      REQUIRE(bb.lower() <= bb.middle());
      REQUIRE(bb.middle() <= bb.upper());
    }
    CHECK(worst < 1e-12);
  }
}

TEST_CASE("differential: RSI vs naive, and always within [0, 100]") {
  for (std::size_t n : kPeriods) {
    CAPTURE(n);
    const auto x = walk(3000, 200 + n);
    Rsi rsi(n);
    double worst = 0;
    for (std::size_t i = 0; i < x.size(); ++i) {
      rsi.update(x[i]);
      if (i < n) continue;  // ready after n+1 inputs, i.e. at index n
      const double ref = rsi_ref(x, i, n);
      worst = std::max(worst, std::fabs(rsi.value() - ref));
      REQUIRE(rsi.value() >= 0.0);
      REQUIRE(rsi.value() <= 100.0);
    }
    CHECK(worst < 1e-9);
  }
}

TEST_CASE("differential: ATR vs naive (gaps included), always >= 0") {
  for (std::size_t n : kPeriods) {
    CAPTURE(n);
    const Ohlc o = make_ohlc(3000, 300 + n);
    Atr atr(n);
    double worst = 0;
    for (std::size_t i = 0; i < o.c.size(); ++i) {
      atr.update(o.h[i], o.l[i], o.c[i]);
      if (i + 1 < n) continue;
      worst = std::max(worst, rel(atr.value(), atr_ref(o, i, n)));
      REQUIRE(atr.value() >= 0.0);
    }
    CHECK(worst < 1e-12);
  }
}

TEST_CASE("differential: rolling max/min (monotonic deque) are EXACT, including ties and expiry boundaries") {
  struct Gen { const char* name; std::vector<double> x; };
  std::vector<Gen> gens;
  gens.push_back({"random walk", walk(3000, 400)});
  {
    Rnd r(401);
    std::vector<double> ties;  // few distinct values => many equal values at window boundaries
    for (int i = 0; i < 3000; ++i) ties.push_back(static_cast<double>(static_cast<int>(r.u() * 4)));
    gens.push_back({"heavy ties", ties});
  }
  {
    std::vector<double> dec, inc, flat(500, 7.0);
    for (int i = 0; i < 500; ++i) { dec.push_back(1000.0 - i); inc.push_back(static_cast<double>(i)); }
    gens.push_back({"strictly decreasing", dec});  // worst case for a naive scan: max is always the oldest element
    gens.push_back({"strictly increasing", inc});
    gens.push_back({"constant", flat});
  }
  for (const Gen& g : gens) {
    CAPTURE(g.name);
    for (std::size_t n : kPeriods) {
      CAPTURE(n);
      RollingMax mx(n);
      RollingMin mn(n);
      for (std::size_t i = 0; i < g.x.size(); ++i) {
        mx.update(g.x[i]);
        mn.update(g.x[i]);
        if (i + 1 < n) continue;
        REQUIRE(mx.value() == max_ref(g.x, i, n));
        REQUIRE(mn.value() == min_ref(g.x, i, n));
      }
    }
  }
}

// =============== edge cases ===============

TEST_CASE("edge: constant, monotone, and period-1 series") {
  {  // constant: RSI is 50 (both averages zero), Bollinger sd 0, ATR from h-l only
    Rsi r(5); Bollinger b(5, 2.0); Sma s(5);
    for (int i = 0; i < 20; ++i) { r.update(42.0); b.update(42.0); s.update(42.0); }
    CHECK(r.value() == 50.0);
    CHECK(b.stddev() == 0.0);
    CHECK(b.upper() == 42.0);
    CHECK(s.value() == doctest::Approx(42.0));
  }
  {  // strictly rising -> RSI 100 ; strictly falling -> RSI 0
    Rsi up(5), down(5);
    for (int i = 0; i < 30; ++i) { up.update(100.0 + i); down.update(100.0 - i); }
    CHECK(up.value() == 100.0);
    CHECK(down.value() == 0.0);
  }
  {  // period 1: SMA(1), EMA(1), rolling extremes are the input itself; Bollinger sd 0; RSI(1) ready after 2 inputs
    Sma s(1); Ema e(1); RollingMax mx(1); RollingMin mn(1); Bollinger b(1, 2.0); Rsi r(1);
    for (double v : {5.0, 3.0, 8.0}) {
      s.update(v); e.update(v); mx.update(v); mn.update(v); b.update(v); r.update(v);
      CHECK(s.value() == v);
      CHECK(e.value() == v);   // alpha = 2/(1+1) = 1
      CHECK(mx.value() == v);
      CHECK(mn.value() == v);
      CHECK(b.stddev() == 0.0);
    }
    CHECK(r.value() == 100.0);  // last change +5: avg_loss 0, avg_gain > 0
  }
}

// =============== numerical stability, measured ===============
// The tolerances below were set from errors MEASURED beforehand (see LEARNING.md): SMA <= 7e-14 relative over
// 1e6 bars; Bollinger sd 1.9e-14 relative (realistic) and 3.1e-11 absolute (price 1e5, sd 0.01), with margin.

TEST_CASE("stability: long series with realistic and extreme level/spread") {
  struct Case { double mean, sd; std::size_t n, N; double sd_abs_tol; };
  const Case cases[] = {{100, 1, 20, 200000, 1e-11}, {5, 0.5, 50, 200000, 1e-11}, {1e5, 0.01, 20, 300000, 1e-9},
                        {1e5, 0.01, 250, 300000, 1e-9}};
  for (const Case& c : cases) {
    CAPTURE(c.mean);
    CAPTURE(c.n);
    Rnd r(11);
    std::vector<double> x(c.N);
    Sma sma(c.n);
    Bollinger bb(c.n, 2.0);
    for (std::size_t i = 0; i < c.N; ++i) {
      x[i] = c.mean + c.sd * (r.u() - 0.5) * 3.4641016151377544;  // uniform with standard deviation c.sd
      sma.update(x[i]);
      bb.update(x[i]);
      if (i + 1 >= c.n && (i % 997 == 0 || i + 1 == c.N)) {
        REQUIRE(rel(sma.value(), sma_ref(x, i, c.n)) < 1e-12);
        REQUIRE(std::fabs(bb.stddev() - pop_sd_ref(x, i, c.n)) < c.sd_abs_tol);
      }
    }
  }
}
