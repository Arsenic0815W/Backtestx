#pragma once
// Incremental technical indicators. Each class is fed one bar at a time with update(...) and costs
// amortised O(1) per update with O(period) memory. (Speed is NOT measured yet; see Phase 7.)
//
// Conventions (pinned here; the tests compare against naive O(n) references written from THESE definitions):
//   * value() returns NaN until ready(). Check ready() first.
//   * SMA(n)        mean of the last n inputs. Ready after n inputs.
//   * EMA(n)        alpha = 2/(n+1). Seeded with the SMA of the first n inputs (that is its first value, at input n);
//                   afterwards ema = alpha*x + (1-alpha)*ema. Ready after n inputs.
//   * RSI(n)        Wilder. change_i = x_i - x_{i-1}. After the first n changes: avg_gain/avg_loss = plain mean of the n
//                   gains / losses. Then avg = (avg*(n-1) + current)/n. RSI = 100 - 100/(1+avg_gain/avg_loss);
//                   avg_loss == 0 gives 100 (or 50 if avg_gain is also 0, i.e. a flat series). Ready after n+1 inputs.
//   * ATR(n)        Wilder. TR_i = max(high-low, |high-prev_close|, |low-prev_close|); the FIRST bar has no previous
//                   close so TR_1 = high-low. ATR is seeded with the mean of the first n TRs, then
//                   atr = (atr*(n-1) + TR)/n. Ready after n bars.
//   * RollingMax/Min(n)   extreme of the last n inputs INCLUDING the newest, via a monotonic deque. Ready after n inputs.
//                   (Donchian channels use "the n bars BEFORE today": read value() before calling update(today).)
//   * Bollinger(n,k) middle = SMA(n); sd = POPULATION standard deviation (divide by n) of the same window;
//                   upper/lower = middle +- k*sd. Ready after n inputs.
//
// Numerical note: Sma keeps a running sum (measured error <= 7e-14 relative over 1e6 bars). Bollinger uses a sliding
// Welford update re-summed exactly every n updates (see LEARNING.md for the measured error before/after that fix).
#include <cstddef>
#include <deque>
#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>

namespace bt {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

class Sma {
 public:
  explicit Sma(std::size_t n);
  void update(double x);
  bool ready() const { return count_ >= n_; }
  double value() const { return ready() ? sum_ / static_cast<double>(n_) : kNaN; }
  std::size_t period() const { return n_; }

 private:
  std::size_t n_, count_ = 0, head_ = 0;
  double sum_ = 0;
  std::vector<double> buf_;
};

class Ema {
 public:
  explicit Ema(std::size_t n);
  void update(double x);
  bool ready() const { return count_ >= n_; }
  double value() const { return ready() ? ema_ : kNaN; }

 private:
  std::size_t n_, count_ = 0;
  double alpha_, sum_ = 0, ema_ = 0;
};

class Rsi {
 public:
  explicit Rsi(std::size_t n);
  void update(double close);
  bool ready() const { return changes_ >= n_; }
  double value() const;

 private:
  std::size_t n_, changes_ = 0;
  bool have_prev_ = false;
  double prev_ = 0, avg_gain_ = 0, avg_loss_ = 0;
};

class Atr {
 public:
  explicit Atr(std::size_t n);
  void update(double high, double low, double close);
  bool ready() const { return count_ >= n_; }
  double value() const { return ready() ? atr_ : kNaN; }

 private:
  std::size_t n_, count_ = 0;
  bool have_prev_ = false;
  double prev_close_ = 0, atr_ = 0;
};

// Monotonic-deque sliding extreme. IsMax=true -> rolling maximum, false -> rolling minimum.
template <bool IsMax>
class RollingExtreme {
 public:
  explicit RollingExtreme(std::size_t n) : n_(n) {
    if (n == 0) throw std::invalid_argument("RollingExtreme: period must be >= 1");
  }
  void update(double x) {
    // Drop dominated entries from the back: they can never be the extreme while x is in the window.
    while (!dq_.empty() && (IsMax ? dq_.back().second <= x : dq_.back().second >= x)) dq_.pop_back();
    dq_.emplace_back(index_, x);
    // Drop the front if it has left the window [index_-n+1, index_].
    if (dq_.front().first + n_ <= index_) dq_.pop_front();
    ++index_;
  }
  bool ready() const { return index_ >= n_; }
  double value() const { return ready() ? dq_.front().second : kNaN; }

 private:
  std::size_t n_, index_ = 0;
  std::deque<std::pair<std::size_t, double>> dq_;
};
using RollingMax = RollingExtreme<true>;
using RollingMin = RollingExtreme<false>;

class Bollinger {
 public:
  Bollinger(std::size_t n, double k);
  void update(double x);
  bool ready() const { return count_ >= n_; }
  double middle() const { return ready() ? mean_ : kNaN; }
  double stddev() const;
  double upper() const { return ready() ? mean_ + k_ * stddev() : kNaN; }
  double lower() const { return ready() ? mean_ - k_ * stddev() : kNaN; }

 private:
  std::size_t n_, count_ = 0, head_ = 0, since_resync_ = 0;
  double k_, mean_ = 0, m2_ = 0;
  std::vector<double> buf_;
};

}  // namespace bt
