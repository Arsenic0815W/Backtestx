#include "bt/adjust.hpp"

#include <algorithm>

namespace bt {

std::vector<Bar> back_adjust(const std::vector<Bar>& bars) {
  std::vector<Bar> out;
  out.reserve(bars.size());
  for (const Bar& b : bars) {
    const double f = b.adj_close / b.close;
    Bar a = b;
    a.open = b.open * f;
    a.close = b.adj_close;
    a.high = std::max({b.high * f, a.open, a.close});
    a.low = std::min({b.low * f, a.open, a.close});
    // adj_close already equals close'.
    out.push_back(a);
  }
  return out;
}

}  // namespace bt
