#pragma once
#include <optional>
#include <vector>

#include "bt/calendar.hpp"
#include "bt/costs.hpp"
#include "bt/order.hpp"
#include "bt/strategy.hpp"

namespace bt {

struct EngineConfig {
  Paise initial_cash = 100'000'000;  // Rs 10,00,000 in paise
  CostConfig costs = CostConfig::nse_delivery_defaults();
  SlippageConfig slippage;
  // Long-only unless true. NOTE: Indian cash-market short selling is intraday-only (delivery shorting does
  // not exist); that restriction, margin and borrow costs are NOT modelled. With the flag on, a short is a
  // simplified "negative position" that may be held across days, charged the sell-side delivery costs.
  bool allow_short = false;
  // 0 = off. Otherwise each fill is capped at floor(fraction * bar volume) shares.
  double max_volume_participation = 0.0;
  // Evaluation start (inclusive). Calendar days BEFORE it are WARM-UP: the strategy's on_bar is called (so indicators and
  // other incremental state see the history; Context::in_warmup() is true) but nothing counts - every order it submits is
  // discarded, no fills/events/equity records are produced, the portfolio stays at initial cash, and order ids restart at 1
  // on the first evaluation day. The first decision that counts is therefore made at the CLOSE of the first evaluation
  // day (and fills from the next bar). Every strategy run with the same eval_start gets the same window. Unset = no warm-up.
  std::optional<Date> eval_start;
};

// One executed (possibly partial) order.
struct Fill {
  OrderId order_id = 0;
  Date date = 0;
  std::size_t day_index = 0;
  std::size_t symbol = 0;
  Side side = Side::Buy;
  std::int64_t qty = 0;
  Paise price = 0;     // per share
  Paise notional = 0;  // price * qty
  FeeBreakdown fees;
  Paise cash_after = 0;
  std::int64_t position_after = 0;
};

// One round trip: from flat to flat in one symbol (possibly several fills in and out).
struct Trade {
  std::size_t symbol = 0;
  Side direction = Side::Buy;  // Buy = long trade, Sell = short trade
  Date entry_date = 0, exit_date = 0;
  std::size_t entry_day = 0, exit_day = 0;  // calendar indices; bars_held = exit_day - entry_day
  std::int64_t qty = 0;                     // total shares opened
  Paise entry_value = 0;                    // sum of opening notionals
  Paise exit_value = 0;                     // sum of closing notionals
  Paise gross_pnl = 0;                      // before fees:  (sell notionals) - (buy notionals)
  FeeBreakdown fees;                        // all fees on all fills of this trade
  Paise net_pnl = 0;                        // gross_pnl - fees.total()
  int num_fills = 0;
};

struct DailyRecord {
  Date date = 0;
  Paise cash = 0;
  Paise positions_value = 0;  // sum(qty * last known close), signed
  Paise equity = 0;           // cash + positions_value
};

struct OpenPosition {
  std::size_t symbol = 0;
  std::int64_t qty = 0;
  Paise cost = 0;  // see Position::cost
  Paise mark = 0;  // last known close, per share
  Paise unrealized_gross = 0;
};

struct EngineResult {
  std::vector<Symbol> symbols;
  Paise initial_cash = 0;
  std::vector<DailyRecord> equity_curve;  // one record per calendar day, after marking to market
  std::vector<Fill> fills;
  std::vector<Trade> trades;              // closed round trips only
  std::vector<OrderEvent> events;         // rejections, expiries, cancellations
  std::vector<OpenPosition> open_positions;  // still held when the data ended (NOT liquidated)
  Paise final_cash = 0;
  Paise final_equity = 0;
  Paise realized_gross = 0;
  FeeBreakdown fees;  // totals over all fills
  std::size_t orders_pending_at_end = 0;
  std::size_t warmup_days = 0;                // calendar days before eval_start (not in equity_curve)
  std::size_t warmup_orders_discarded = 0;    // orders the strategy submitted during warm-up and that were thrown away
};

// Event-driven simulator. For each calendar day t, in this fixed order:
//   1. FILL     pending orders are tested against day t's bar (bar prices only; see fill.hpp)
//   2. MARK     every position is valued at its last known close; the equity record for day t is written
//   3. DECIDE   strategy.on_bar(ctx) sees history up to and including day t's close
//   4. COLLECT  orders submitted in step 3 join the pending list (they cannot fill before day t+1)
// so a decision made on day t's close can execute at day t+1's open at the earliest.
//
// Processing order WITHIN step 1 (deterministic; documented because it matters):
//   Market orders first, then Stop orders, then Limit orders; ties broken by order id (submission order).
//   * a market entry and its protective stop submitted together: entry (market) is processed before the stop;
//   * rebalances: sizing helpers submit sells before buys, so ascending id funds buys with sale proceeds;
//   * if a stop and a limit on the same symbol are both touched in one bar, OHLC cannot tell which came
//     first, so the stop (the adverse outcome) is processed first. The limit is then usually rejected
//     because the position is gone. There is no OCO linking: cancel the sibling yourself.
//
// Missing bars: if a symbol has no bar on day t its pending orders cannot fill; Day orders expire anyway,
// GTC orders wait. Marking uses the last real close.
// Start/end: on_start gets no data and cannot trade. Orders submitted on the final bar never fill. Open
// positions at the end are marked, not liquidated.
// The Engine holds no mutable global state: run() uses only the const AlignedData and its own locals,
// so independent runs can execute on separate threads.
// TODO(Phase 5): this thread-safety is argued from the code, NOT yet tested; see TODO.md (concurrent-run test).
class Engine {
 public:
  Engine(const AlignedData& data, EngineConfig cfg) : data_(data), cfg_(std::move(cfg)) {}
  EngineResult run(Strategy& strategy) const;

 private:
  const AlignedData& data_;
  EngineConfig cfg_;
};

}  // namespace bt
