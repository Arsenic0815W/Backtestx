#pragma once
#include <optional>

#include "bt/bar.hpp"
#include "bt/costs.hpp"
#include "bt/order.hpp"

namespace bt {

// A bar with prices rounded to the nearest paisa. All fill logic runs on these integers, so every
// comparison ("did the low reach my limit?") is exact and hand-checkable.
struct BarP {
  Paise open = 0, high = 0, low = 0, close = 0;
  std::int64_t volume = 0;
};

BarP to_paise_bar(const Bar& b);

struct FillQuote {
  std::int64_t qty;
  Paise price;  // per share, paise
};

// Pure function: given ONE pending order and the bar it is being tested against, decide whether it
// executes, how many shares, and at what price. It knows nothing about cash or positions (the engine
// applies those checks afterwards). nullopt == no execution on this bar.
//
// Rules (OHLC only; the order of events inside a bar is unknown, so every rule is a stated convention):
//
//   Market      fills at the OPEN, then adverse slippage.
//   Limit buy   fills if low  <= limit, at min(open, limit)   (gap below the limit fills at the better open)
//   Limit sell  fills if high >= limit, at max(open, limit)   (gap above the limit fills at the better open)
//               "Touching" the limit exactly counts as a fill (optimistic: real queues may not fill you).
//               No slippage on limits.
//   Stop buy    triggers if high >= stop, at max(open, stop)  (gap above the stop fills at the open)
//   Stop sell   triggers if low  <= stop, at min(open, stop)  (gap below the stop fills at the open)
//               then adverse slippage, as a market order would get.
//
//   Slipped prices are clamped into [low, high]: you cannot trade outside the bar's traded range.
//
//   volume_participation (0 = off): the fill quantity is capped at floor(fraction * bar volume).
//   A zero cap quantity means no fill. The unfilled remainder stays with the order (see TimeInForce).
std::optional<FillQuote> try_fill(const Order& o, const BarP& bar, const SlippageConfig& slip,
                                  double volume_participation);

}  // namespace bt
