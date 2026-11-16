#pragma once
// Built-in strategies, created by name from strict JSON parameters.
//
//   buy_and_hold         benchmark: buy once, never sell
//   sma_crossover        long while fast SMA > slow SMA, else flat
//   rsi_mean_reversion   buy oversold, sell overbought
//   donchian_breakout    channel breakout entry, channel exit, ATR protective stop
//   momentum_rotation    monthly: hold the top-K of a universe by N-bar return, equal weight
//
// Common rules for all of them:
//   * Long-only. Entries and exits are DAY market orders (so a rejected or expired order is simply re-derived
//     from the state on the next bar). The only GTC orders are the Donchian protective stops.
//   * A single-symbol strategy trades the symbol named by the "symbol" parameter (default: the first symbol in the
//     data); other symbols in the data (e.g. a benchmark) are ignored. Momentum uses "universe".
//   * "allocation" (0 < a <= 1) is the fraction of current equity committed to an entry. Default 0.95, except
//     buy_and_hold which defaults to 0.99 (the benchmark should carry little cash drag). Sizes are computed from the
//     signal bar's CLOSE but fill at the NEXT open plus fees, so allocation < 1 leaves headroom against
//     InsufficientCash rejections after a gap up (about 5 % at 0.95, about 1 % at 0.99); a rejected entry is
//     retried, re-sized, on the next bar.
//   * Indicators are updated only on days the symbol actually has a NEW bar. A symbol with a hole that day is skipped.
//   * Warm-up: nothing trades until the indicators are ready (BuyAndHold starts on day 0). Full-period statistics
//     of strategies with different warm-ups are therefore not directly comparable (exposure differs).
//   * Parameters are validated when the strategy is CREATED (ConfigError), never in the middle of a run; an unknown
//     symbol name is reported at on_start. Orders are only submitted with positive quantity and prices.
//
// Each strategy's idea, exact rules and known weaknesses are documented in LEARNING.md (Phase 3).
#include <memory>
#include <string>
#include <vector>

#include "bt/strategy.hpp"
#include "json.hpp"

namespace bt {

// Throws ConfigError for an unknown strategy name, unknown parameter keys, wrong types or invalid values
// (e.g. fast >= slow). Each call returns a FRESH instance: strategies hold state and must never be shared
// between runs or threads.
std::unique_ptr<Strategy> make_strategy(const std::string& name, const nlohmann::json& params);

const std::vector<std::string>& strategy_names();

}  // namespace bt
