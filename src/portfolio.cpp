#include "bt/portfolio.hpp"

#include <cstdlib>
#include <stdexcept>

namespace bt {

Portfolio::Portfolio(std::size_t num_symbols, Paise initial_cash)
    : cash_(initial_cash), initial_cash_(initial_cash), pos_(num_symbols) {
  if (initial_cash < 0) throw std::logic_error("Portfolio: negative initial cash");
}

double Portfolio::avg_cost(std::size_t s) const {
  const Position& p = pos_[s];
  if (p.qty == 0) return 0.0;
  return to_rupees(p.cost) / static_cast<double>(p.qty);  // cost and qty share a sign -> positive
}

Paise Portfolio::apply_fill(std::size_t s, Side side, std::int64_t qty, Paise price, Paise fees) {
  if (qty <= 0 || price <= 0 || fees < 0) throw std::logic_error("Portfolio::apply_fill: bad fill arguments");
  Position& p = pos_[s];
  const Paise notional = price * qty;
  const std::int64_t signed_qty = side == Side::Buy ? qty : -qty;
  // Validate everything BEFORE mutating, so a thrown logic_error never leaves a half-applied fill.
  const Paise new_cash = cash_after(side, notional, fees);
  if (new_cash < 0) throw std::logic_error("Portfolio::apply_fill: cash would go negative");
  const bool opens_or_adds = p.qty == 0 || (p.qty > 0) == (signed_qty > 0);

  Paise realised = 0;
  if (opens_or_adds) {
    p.qty += signed_qty;
    p.cost += side == Side::Buy ? notional : -notional;
  } else {
    const std::int64_t held = std::llabs(p.qty);
    if (qty > held) throw std::logic_error("Portfolio::apply_fill: fill crosses zero");
    const Paise released = (qty == held) ? p.cost : mul_div_round(p.cost, qty, held);
    // Cash flow of the closing fill: a sell brings +notional, a buy costs -notional.
    realised = (side == Side::Sell ? notional : -notional) - released;
    p.qty += signed_qty;
    p.cost -= released;
    if (p.qty == 0 && p.cost != 0) throw std::logic_error("Portfolio::apply_fill: residual cost on flat position");
  }

  cash_ = new_cash;
  fees_ += fees;
  realized_ += realised;
  return realised;
}

}  // namespace bt
