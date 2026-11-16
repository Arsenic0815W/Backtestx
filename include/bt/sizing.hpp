#pragma once
#include <cstdint>
#include <vector>

#include "bt/strategy.hpp"

namespace bt {

// Position-sizing helpers. They are plain functions of numbers (easy to test) plus two that talk to a Context.
//
// IMPORTANT LIMITATION: sizes are computed from the latest CLOSE you can see, but the order fills at the NEXT
// open (plus fees and slippage). A buy sized to "all my cash" can therefore be rejected for InsufficientCash if
// the open gaps up or fees push it over. Use the buffer_pct argument (or size below 100 %) to leave headroom.
// All helpers round DOWN to whole shares.

std::int64_t qty_fixed(std::int64_t qty);                                    // returns max(qty, 0)
std::int64_t qty_for_amount(double rupees, double price);                    // floor(rupees / price)
std::int64_t qty_for_equity_pct(double equity_rupees, double pct, double price);  // pct is percent (10 = 10 %)

struct TargetWeight {
  std::size_t symbol;
  double weight;  // fraction of equity, 0..1 (long-only use)
};

// Brings holdings to the target weights with market orders, SELLS FIRST (ascending symbol index) and then
// BUYS (ascending), so that with ascending-id processing the sale proceeds are available to the buys.
//   * any symbol currently held but not listed is sold in full (target weight 0);
//   * symbols with no visible bar yet are skipped;
//   * target_qty = floor(equity * (1 - buffer_pct/100) * weight / latest_close).
// Returns the ids of the orders submitted, in submission order. Call from on_bar only.
std::vector<OrderId> rebalance_to_weights(Context& ctx, const std::vector<TargetWeight>& targets,
                                          double buffer_pct = 0.0);

// Equal weights 1/N over `symbols`.
std::vector<OrderId> rebalance_equal_weight(Context& ctx, const std::vector<std::size_t>& symbols,
                                            double buffer_pct = 0.0);

}  // namespace bt
