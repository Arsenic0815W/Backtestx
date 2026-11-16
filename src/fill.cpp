#include "bt/fill.hpp"

#include <algorithm>

namespace bt {

const char* to_string(RejectReason r) {
  switch (r) {
    case RejectReason::InsufficientCash: return "InsufficientCash";
    case RejectReason::ShortingDisabled: return "ShortingDisabled";
    case RejectReason::CrossesZero: return "CrossesZero";
  }
  return "Unknown";
}

BarP to_paise_bar(const Bar& b) {
  return BarP{to_paise(b.open), to_paise(b.high), to_paise(b.low), to_paise(b.close), b.volume};
}

std::optional<FillQuote> try_fill(const Order& o, const BarP& bar, const SlippageConfig& slip,
                                  double volume_participation) {
  const bool buy = o.side == Side::Buy;
  Paise px = 0;
  bool slipped = false;

  switch (o.type) {
    case OrderType::Market:
      px = bar.open;
      slipped = true;
      break;
    case OrderType::Limit:
      if (buy) {
        if (bar.low > o.price) return std::nullopt;
        px = std::min(bar.open, o.price);
      } else {
        if (bar.high < o.price) return std::nullopt;
        px = std::max(bar.open, o.price);
      }
      break;
    case OrderType::Stop:
      if (buy) {
        if (bar.high < o.price) return std::nullopt;
        px = std::max(bar.open, o.price);
      } else {
        if (bar.low > o.price) return std::nullopt;
        px = std::min(bar.open, o.price);
      }
      slipped = true;
      break;
  }

  if (slipped) {
    const Paise s = slippage_amount(slip, px, bar.high - bar.low);
    px = buy ? px + s : px - s;
    px = std::min(std::max(px, bar.low), bar.high);
  }

  std::int64_t qty = o.remaining;
  if (volume_participation > 0.0) {
    const std::int64_t ppb = static_cast<std::int64_t>(std::llround(volume_participation * 1e9));
    const std::int64_t cap = static_cast<std::int64_t>(static_cast<__int128>(bar.volume) * ppb / 1'000'000'000);
    qty = std::min(qty, cap);
  }
  if (qty <= 0) return std::nullopt;
  return FillQuote{qty, px};
}

}  // namespace bt
