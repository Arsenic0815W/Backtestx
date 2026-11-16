#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

#include "bt/money.hpp"
#include "bt/types.hpp"

namespace bt {

struct Position {
  std::int64_t qty = 0;  // signed: > 0 long, < 0 short
  // Signed cost of the open shares, EXCLUDING fees: sum(buy notional) for a long, -sum(short-sale proceeds)
  // for a short. Reduced proportionally when the position is partly closed.
  Paise cost = 0;
};

// The ledger. Pure bookkeeping: it applies fills the engine has already approved and throws
// std::logic_error if an invariant would break (that means an engine bug, not a data problem).
//
// Definitions (all exact int64 paise):
//   realised (gross) P&L  = proceeds of closed shares - cost basis released for them, before fees
//   unrealised (gross)    = qty * mark - cost basis of the open shares, before fees
//   fees                  = tracked separately; they reduce cash the moment they are paid
// Identity that always holds:  equity - initial_cash == realised + unrealised - fees_paid
class Portfolio {
 public:
  Portfolio(std::size_t num_symbols, Paise initial_cash);

  Paise cash() const { return cash_; }
  Paise initial_cash() const { return initial_cash_; }
  std::int64_t qty(std::size_t s) const { return pos_[s].qty; }
  const Position& position(std::size_t s) const { return pos_[s]; }
  std::size_t num_symbols() const { return pos_.size(); }
  Paise realized_gross() const { return realized_; }
  Paise fees_paid() const { return fees_; }

  // Average cost per share in rupees (0 when flat).
  double avg_cost(std::size_t s) const;
  Paise unrealized_gross(std::size_t s, Paise mark) const { return pos_[s].qty * mark - pos_[s].cost; }

  // Cash this fill would leave behind (may be negative; the engine uses it to reject).
  Paise cash_after(Side side, Paise notional, Paise fees) const {
    return side == Side::Buy ? cash_ - notional - fees : cash_ + notional - fees;
  }

  // Apply an approved fill. Throws std::logic_error if cash would go negative or the fill would cross zero.
  // Returns the realised gross P&L booked by this fill (0 when it only opens or adds).
  Paise apply_fill(std::size_t s, Side side, std::int64_t qty, Paise price, Paise fees);

 private:
  Paise cash_;
  Paise initial_cash_;
  Paise realized_ = 0;
  Paise fees_ = 0;
  std::vector<Position> pos_;
};

}  // namespace bt
