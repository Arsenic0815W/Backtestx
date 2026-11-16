#pragma once
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "bt/bar.hpp"

namespace bt {

// Several symbols placed on one shared trading calendar.
//
// Calendar = sorted union of every date on which at least one symbol has a bar.
// Missing-bar policy (deliberate, and documented in LEARNING.md):
//   * If a symbol has no bar on a calendar day, bar(s, t) returns nullptr. The symbol did not trade
//     that day: no fills are possible and nothing is invented.
//   * No bar is ever synthesised or forward-filled into the data.
//   * For mark-to-market the engine may ask last_bar(s, t) - the most recent REAL bar at or before
//     day t - which is a separate, explicit accessor.
// Covers late listings (leading gap), holidays specific to one symbol (mid gap) and early endings.
class AlignedData {
 public:
  AlignedData() = default;

  std::size_t num_days() const { return dates_.size(); }
  std::size_t num_symbols() const { return series_.size(); }
  const std::vector<Date>& dates() const { return dates_; }
  Date date(std::size_t t) const { return dates_[t]; }
  const Symbol& symbol(std::size_t s) const { return series_[s].symbol; }
  std::optional<std::size_t> symbol_index(const Symbol& sym) const;

  // The symbol's own bars (no gaps filled), strictly increasing by date.
  const std::vector<Bar>& bars(std::size_t s) const { return series_[s].bars; }

  // Bar of symbol s on calendar day t, or nullptr if it did not trade that day.
  const Bar* bar(std::size_t s, std::size_t t) const;

  // Most recent real bar of symbol s at or before calendar day t, or nullptr before its first bar.
  const Bar* last_bar(std::size_t s, std::size_t t) const;

  // How many of symbol s's own bars have a date <= calendar day t (0 before its first bar). This is the
  // size of the history a strategy standing at day t is allowed to see.
  std::size_t num_visible(std::size_t s, std::size_t t) const { return static_cast<std::size_t>(last_[s][t] + 1); }

 private:
  friend AlignedData align_calendar(std::vector<SymbolSeries> series);

  std::vector<Date> dates_;
  std::vector<SymbolSeries> series_;
  // exact_[s][t]: index into series_[s].bars of the bar dated dates_[t], or kNone.
  // last_[s][t]:  index of the latest bar dated <= dates_[t], or kNone.
  std::vector<std::vector<std::int32_t>> exact_;
  std::vector<std::vector<std::int32_t>> last_;
  static constexpr std::int32_t kNone = -1;
};

// Throws std::invalid_argument on: empty or duplicate symbol names, or a series whose dates are not
// strictly increasing. Symbol order is preserved. An empty list or empty series are allowed.
AlignedData align_calendar(std::vector<SymbolSeries> series);

}  // namespace bt
