#include "bt/indicators.hpp"

#include <algorithm>
#include <cmath>
#include <string>

namespace bt {

namespace {
void need_period(std::size_t n, const char* who) {
  if (n == 0) throw std::invalid_argument(std::string(who) + ": period must be >= 1");
}
}  // namespace

// ---------------- SMA ----------------
Sma::Sma(std::size_t n) : n_(n), buf_(n) { need_period(n, "Sma"); }

void Sma::update(double x) {
  if (count_ < n_) {
    ++count_;
    sum_ += x;
  } else {
    sum_ += x - buf_[head_];  // newest in, oldest out
  }
  buf_[head_] = x;
  head_ = (head_ + 1) % n_;
}

// ---------------- EMA ----------------
Ema::Ema(std::size_t n) : n_(n), alpha_(2.0 / (static_cast<double>(n) + 1.0)) { need_period(n, "Ema"); }

void Ema::update(double x) {
  if (count_ < n_) {
    ++count_;
    sum_ += x;
    if (count_ == n_) ema_ = sum_ / static_cast<double>(n_);  // seed = SMA of the first n inputs
  } else {
    ema_ = alpha_ * x + (1.0 - alpha_) * ema_;
  }
}

// ---------------- RSI (Wilder) ----------------
Rsi::Rsi(std::size_t n) : n_(n) { need_period(n, "Rsi"); }

void Rsi::update(double close) {
  if (!have_prev_) {
    have_prev_ = true;
    prev_ = close;
    return;
  }
  const double change = close - prev_;
  prev_ = close;
  const double gain = change > 0 ? change : 0.0;
  const double loss = change < 0 ? -change : 0.0;
  const double n = static_cast<double>(n_);
  if (changes_ < n_) {
    avg_gain_ += gain;
    avg_loss_ += loss;
    ++changes_;
    if (changes_ == n_) {
      avg_gain_ /= n;
      avg_loss_ /= n;
    }
  } else {
    avg_gain_ = (avg_gain_ * (n - 1.0) + gain) / n;
    avg_loss_ = (avg_loss_ * (n - 1.0) + loss) / n;
  }
}

double Rsi::value() const {
  if (!ready()) return kNaN;
  if (avg_loss_ == 0.0) return avg_gain_ == 0.0 ? 50.0 : 100.0;
  return 100.0 - 100.0 / (1.0 + avg_gain_ / avg_loss_);
}

// ---------------- ATR (Wilder) ----------------
Atr::Atr(std::size_t n) : n_(n) { need_period(n, "Atr"); }

void Atr::update(double high, double low, double close) {
  double tr = high - low;
  if (have_prev_) tr = std::max({tr, std::fabs(high - prev_close_), std::fabs(low - prev_close_)});
  have_prev_ = true;
  prev_close_ = close;
  const double n = static_cast<double>(n_);
  if (count_ < n_) {
    atr_ += tr;
    ++count_;
    if (count_ == n_) atr_ /= n;
  } else {
    atr_ = (atr_ * (n - 1.0) + tr) / n;
  }
}

// ---------------- Bollinger ----------------
Bollinger::Bollinger(std::size_t n, double k) : n_(n), k_(k), buf_(n) {
  need_period(n, "Bollinger");
  if (!(k >= 0.0)) throw std::invalid_argument("Bollinger: k must be >= 0");
}

void Bollinger::update(double x) {
  if (count_ < n_) {  // filling: plain Welford
    ++count_;
    const double delta = x - mean_;
    mean_ += delta / static_cast<double>(count_);
    m2_ += delta * (x - mean_);
  } else {  // full: slide the window by replacing the oldest value
    const double old = buf_[head_];
    const double old_mean = mean_;
    mean_ += (x - old) / static_cast<double>(n_);
    m2_ += (x - old) * (x - mean_ + old - old_mean);
    if (m2_ < 0.0) m2_ = 0.0;  // rounding can push a tiny variance below zero
  }
  buf_[head_] = x;
  head_ = (head_ + 1) % n_;
  // Every n updates re-sum the window exactly (two passes over n values): O(n) work once per n updates is
  // amortised O(1), and it stops the sliding update's rounding error from accumulating over a long series.
  if (count_ >= n_ && ++since_resync_ >= n_) {
    since_resync_ = 0;
    double sum = 0.0;
    for (double v : buf_) sum += v;
    mean_ = sum / static_cast<double>(n_);
    m2_ = 0.0;
    for (double v : buf_) m2_ += (v - mean_) * (v - mean_);
  }
}

double Bollinger::stddev() const {
  if (!ready()) return kNaN;
  return std::sqrt(std::max(m2_, 0.0) / static_cast<double>(n_));
}

}  // namespace bt
