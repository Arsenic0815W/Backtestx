#pragma once
// Shared helpers for engine-level tests.
#include <functional>
#include <vector>

#include "bt/calendar.hpp"
#include "bt/engine.hpp"

namespace tu {

using namespace bt;

inline Bar mk(Date d, double o, double h, double l, double c, std::int64_t vol = 1'000'000) {
  return Bar{d, o, h, l, c, c, vol};
}

// All charges zero: P&L then equals plain price arithmetic.
inline CostConfig free_costs() {
  CostConfig c;
  c.brokerage_flat_rupees = 0;
  c.stt_buy_pct = c.stt_sell_pct = c.exchange_txn_pct = c.sebi_pct = c.stamp_buy_pct = c.gst_pct = 0;
  return c;
}

// Round-number rates used in the hand-computed scenarios (see test_money_costs.cpp):
// flat Rs 20, STT 0.1 % both sides, exchange 0.003 %, SEBI 0.0001 %, stamp 0.015 % buy, GST 18 %.
inline CostConfig hand_costs() {
  CostConfig c;
  c.brokerage_flat_rupees = 20;
  c.stt_buy_pct = c.stt_sell_pct = 0.1;
  c.exchange_txn_pct = 0.003;
  c.sebi_pct = 0.0001;
  c.stamp_buy_pct = 0.015;
  c.gst_pct = 18;
  return c;
}

inline EngineConfig engine_cfg(Paise cash_paise, const CostConfig& costs) {
  EngineConfig c;
  c.initial_cash = cash_paise;
  c.costs = costs;
  return c;
}

// A strategy defined by lambdas.
struct Scripted : Strategy {
  std::function<void(Context&)> bar_fn;
  std::function<void(Context&)> start_fn;
  std::function<void(Context&)> end_fn;
  explicit Scripted(std::function<void(Context&)> f) : bar_fn(std::move(f)) {}
  void on_start(Context& c) override { if (start_fn) start_fn(c); }
  void on_bar(Context& c) override { bar_fn(c); }
  void on_end(Context& c) override { if (end_fn) end_fn(c); }
};

inline AlignedData one_symbol(const std::vector<Bar>& bars, const char* name = "X") {
  return align_calendar({{name, bars}});
}

// n identical flat bars on dates 100..100+n-1.
inline std::vector<Bar> flat_bars(int n, double o = 1000, double h = 1010, double l = 990, double c = 1000,
                                  std::int64_t vol = 1'000'000) {
  std::vector<Bar> v;
  for (int i = 0; i < n; ++i) v.push_back(mk(100 + i, o, h, l, c, vol));
  return v;
}

inline int count_events(const EngineResult& r, OrderEventKind k) {
  int n = 0;
  for (const auto& e : r.events) n += e.kind == k;
  return n;
}

}  // namespace tu
