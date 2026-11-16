#include "bt/calendar.hpp"

#include <algorithm>
#include <set>
#include <stdexcept>
#include <string>

namespace bt {

std::optional<std::size_t> AlignedData::symbol_index(const Symbol& sym) const {
  for (std::size_t i = 0; i < series_.size(); ++i)
    if (series_[i].symbol == sym) return i;
  return std::nullopt;
}

const Bar* AlignedData::bar(std::size_t s, std::size_t t) const {
  const std::int32_t i = exact_[s][t];
  return i == kNone ? nullptr : &series_[s].bars[static_cast<std::size_t>(i)];
}

const Bar* AlignedData::last_bar(std::size_t s, std::size_t t) const {
  const std::int32_t i = last_[s][t];
  return i == kNone ? nullptr : &series_[s].bars[static_cast<std::size_t>(i)];
}

AlignedData align_calendar(std::vector<SymbolSeries> series) {
  std::set<Symbol> seen;
  for (const SymbolSeries& s : series) {
    if (s.symbol.empty()) throw std::invalid_argument("align_calendar: empty symbol name");
    if (!seen.insert(s.symbol).second) throw std::invalid_argument("align_calendar: duplicate symbol " + s.symbol);
    for (std::size_t i = 1; i < s.bars.size(); ++i)
      if (s.bars[i].date <= s.bars[i - 1].date)
        throw std::invalid_argument("align_calendar: dates not strictly increasing in " + s.symbol);
  }

  AlignedData out;
  std::vector<Date> all;
  for (const SymbolSeries& s : series)
    for (const Bar& b : s.bars) all.push_back(b.date);
  std::sort(all.begin(), all.end());
  all.erase(std::unique(all.begin(), all.end()), all.end());
  out.dates_ = std::move(all);

  const std::size_t n = out.dates_.size();
  out.exact_.assign(series.size(), std::vector<std::int32_t>(n, AlignedData::kNone));
  out.last_.assign(series.size(), std::vector<std::int32_t>(n, AlignedData::kNone));
  for (std::size_t s = 0; s < series.size(); ++s) {
    const std::vector<Bar>& bars = series[s].bars;
    std::size_t j = 0;                       // next bar of this symbol not yet placed
    std::int32_t last = AlignedData::kNone;  // latest placed bar
    for (std::size_t t = 0; t < n; ++t) {
      if (j < bars.size() && bars[j].date == out.dates_[t]) {
        last = static_cast<std::int32_t>(j);
        out.exact_[s][t] = last;
        ++j;
      }
      out.last_[s][t] = last;
    }
  }
  out.series_ = std::move(series);
  return out;
}

}  // namespace bt
