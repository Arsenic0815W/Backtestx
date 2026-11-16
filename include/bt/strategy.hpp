#pragma once
#include <cstddef>
#include <optional>
#include <stdexcept>
#include <vector>

#include "bt/bar.hpp"
#include "bt/calendar.hpp"
#include "bt/order.hpp"
#include "bt/portfolio.hpp"

namespace bt {

// Read-only window onto ONE symbol's bars up to and including the current day. It stores the visible
// count, not the series: operator[] bounds-checks against that count and throws std::out_of_range
// beyond it. There is no iterator, no data() and no way to obtain a pointer to a later bar.
//
// Guarantee level: this is an API-level guarantee (strategy code written against this interface cannot
// reach future bars). It is NOT memory-safety against hostile code that casts pointers around.
//
// Index convention: view[0] is the most recent visible bar, view[1] the one before it, ...
// If the symbol did not trade today, view[0] is its last real bar (see traded_today() on the Context).
class HistoryView {
 public:
  std::size_t size() const { return n_; }
  bool empty() const { return n_ == 0; }

  const Bar& operator[](std::size_t ago) const { return at(ago); }
  const Bar& at(std::size_t ago) const {
    if (ago >= n_) throw std::out_of_range("HistoryView: index is beyond the visible history");
    return (*bars_)[n_ - 1 - ago];
  }
  const Bar& latest() const { return at(0); }

 private:
  friend class Context;
  HistoryView(const std::vector<Bar>* bars, std::size_t n) : bars_(bars), n_(n) {}
  const std::vector<Bar>* bars_;
  std::size_t n_;
};

enum class Phase { Start, Bar, End };

// What a strategy sees and can do. Constructed only by the Engine.
//   * on_bar: data up to and including today's close; orders may be submitted (they fill from tomorrow).
//   * on_start: no data (every history is empty), orders not allowed.
//   * on_end:   all data visible, orders not allowed (the run is over).
class Context {
 public:
  Phase phase() const { return phase_; }
  // True on warm-up days (before EngineConfig::eval_start): on_bar runs so incremental state can warm up, but orders are
  // discarded. A strategy with state that would be corrupted by acting on discarded orders (e.g. "this month is done")
  // must not commit that state while this is true.
  bool in_warmup() const { return warmup_; }

  // ---- time ----
  Date date() const;                    // today's date (Bar/End phases)
  std::size_t day_index() const;        // 0-based index of today on the shared calendar

  // ---- universe ----
  std::size_t num_symbols() const { return data_->num_symbols(); }
  const Symbol& symbol_name(std::size_t s) const { return data_->symbol(s); }
  std::optional<std::size_t> symbol_index(const Symbol& name) const { return data_->symbol_index(name); }

  // ---- market data (history only) ----
  HistoryView history(std::size_t s) const;
  bool traded_today(std::size_t s) const;  // did symbol s have a bar exactly today?

  // ---- account ----
  Paise cash_paise() const { return pf_->cash(); }
  double cash() const { return to_rupees(pf_->cash()); }
  Paise equity_paise() const { return equity_; }  // marked at today's close
  double equity() const { return to_rupees(equity_); }
  std::int64_t position(std::size_t s) const { return pf_->qty(s); }
  double avg_cost(std::size_t s) const { return pf_->avg_cost(s); }

  // ---- orders (Bar phase only) ----
  // Prices are in rupees; they are rounded to the nearest paisa. qty must be > 0.
  // Returns the order id. Throws std::invalid_argument for a malformed order, std::logic_error outside on_bar.
  // Orders submitted now are staged and become pending only after on_bar returns; the earliest they
  // can execute is tomorrow's bar.
  OrderId market(std::size_t s, Side side, std::int64_t qty, TimeInForce tif = TimeInForce::Day);
  OrderId limit(std::size_t s, Side side, std::int64_t qty, double price, TimeInForce tif = TimeInForce::Day);
  OrderId stop(std::size_t s, Side side, std::int64_t qty, double price, TimeInForce tif = TimeInForce::Day);
  OrderId buy(std::size_t s, std::int64_t qty) { return market(s, Side::Buy, qty); }
  OrderId sell(std::size_t s, std::int64_t qty) { return market(s, Side::Sell, qty); }

  // Cancel a pending or staged order. Takes effect before the next bar, so no look-ahead is possible.
  bool cancel(OrderId id);
  std::size_t cancel_all(std::size_t s);  // returns how many were cancelled
  std::vector<Order> open_orders(std::size_t s) const;  // pending + staged, for symbol s

 private:
  friend class Engine;
  Context() = default;

  OrderId submit(std::size_t s, Side side, OrderType type, std::int64_t qty, double price, TimeInForce tif);

  Phase phase_ = Phase::Start;
  bool warmup_ = false;
  const AlignedData* data_ = nullptr;
  const Portfolio* pf_ = nullptr;
  std::size_t t_ = 0;
  Paise equity_ = 0;
  OrderId* next_id_ = nullptr;
  std::vector<Order>* pending_ = nullptr;
  std::vector<Order>* staged_ = nullptr;
  std::vector<OrderEvent>* events_ = nullptr;
};

class Strategy {
 public:
  virtual ~Strategy() = default;
  virtual void on_start(Context&) {}
  virtual void on_bar(Context& ctx) = 0;
  virtual void on_end(Context&) {}
};

}  // namespace bt
