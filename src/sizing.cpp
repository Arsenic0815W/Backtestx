#include "bt/sizing.hpp"

#include <algorithm>
#include <cmath>

namespace bt {

std::int64_t qty_fixed(std::int64_t qty) { return std::max<std::int64_t>(qty, 0); }

std::int64_t qty_for_amount(double rupees, double price) {
  if (!(rupees > 0.0) || !(price > 0.0)) return 0;
  // 1e-9 relative nudge so that e.g. 10000/100 never becomes 99 through floating-point noise.
  return static_cast<std::int64_t>(std::floor(rupees / price * (1.0 + 1e-12)));
}

std::int64_t qty_for_equity_pct(double equity_rupees, double pct, double price) {
  return qty_for_amount(equity_rupees * pct / 100.0, price);
}

std::vector<OrderId> rebalance_to_weights(Context& ctx, const std::vector<TargetWeight>& targets,
                                          double buffer_pct) {
  const std::size_t n = ctx.num_symbols();
  std::vector<double> weight(n, 0.0);
  for (const TargetWeight& t : targets) {
    if (t.symbol >= n) throw std::invalid_argument("rebalance: bad symbol index");
    weight[t.symbol] = t.weight;
  }
  const double investable = ctx.equity() * (1.0 - buffer_pct / 100.0);

  std::vector<std::int64_t> delta(n, 0);
  for (std::size_t s = 0; s < n; ++s) {
    const HistoryView h = ctx.history(s);
    if (h.empty()) continue;
    const std::int64_t target = qty_for_amount(investable * weight[s], h.latest().close);
    delta[s] = target - ctx.position(s);
  }

  std::vector<OrderId> ids;
  for (std::size_t s = 0; s < n; ++s)
    if (delta[s] < 0) ids.push_back(ctx.sell(s, -delta[s]));
  for (std::size_t s = 0; s < n; ++s)
    if (delta[s] > 0) ids.push_back(ctx.buy(s, delta[s]));
  return ids;
}

std::vector<OrderId> rebalance_equal_weight(Context& ctx, const std::vector<std::size_t>& symbols,
                                            double buffer_pct) {
  std::vector<TargetWeight> t;
  for (std::size_t s : symbols) t.push_back({s, 1.0 / static_cast<double>(symbols.size())});
  return rebalance_to_weights(ctx, t, buffer_pct);
}

}  // namespace bt
