#pragma once
#include <string>
#include <string_view>

#include "bt/money.hpp"
#include "bt/types.hpp"
#include "json.hpp"

namespace bt {

// Each charge is a separate integer number of paise, rounded individually (half away from zero).
struct FeeBreakdown {
  Paise brokerage = 0;
  Paise stt = 0;       // Securities Transaction Tax
  Paise exchange = 0;  // exchange transaction charge (incl. IPFT contribution)
  Paise sebi = 0;      // SEBI turnover fee
  Paise stamp = 0;     // stamp duty (buy side only)
  Paise gst = 0;       // GST on (brokerage + exchange + SEBI), computed on the ROUNDED components

  Paise total() const { return brokerage + stt + exchange + sebi + stamp + gst; }
  FeeBreakdown& operator+=(const FeeBreakdown& o) {
    brokerage += o.brokerage; stt += o.stt; exchange += o.exchange; sebi += o.sebi; stamp += o.stamp; gst += o.gst;
    return *this;
  }
  bool operator==(const FeeBreakdown& o) const {
    return brokerage == o.brokerage && stt == o.stt && exchange == o.exchange && sebi == o.sebi &&
           stamp == o.stamp && gst == o.gst;
  }
};

enum class BrokerageType {
  Flat,     // brokerage_flat_rupees per executed fill (a partial fill pays it again)
  Percent,  // brokerage_pct % of notional, optionally clamped to [min_rupees, max_rupees]
};

// Delivery-equity (CNC) cost model for NSE. RATES CHANGE: check the sources in configs/costs_nse_delivery.json
// before relying on any number. All *_pct fields are PERCENT (0.1 means 0.1 %).
struct CostConfig {
  BrokerageType brokerage_type = BrokerageType::Flat;
  double brokerage_flat_rupees = 0.0;   // default Rs 0: many discount brokers charge nothing on delivery (see configs/)
  double brokerage_pct = 0.0;
  double brokerage_min_rupees = 0.0;
  double brokerage_max_rupees = 0.0;  // 0 = no cap

  double stt_buy_pct = 0.1;
  double stt_sell_pct = 0.1;
  double exchange_txn_pct = 0.00307;  // 306.99 + 0.01 (IPFT) per crore
  double sebi_pct = 0.0001;           // Rs 10 per crore
  double stamp_buy_pct = 0.015;
  double gst_pct = 18.0;

  // The values above; identical to configs/costs_nse_delivery.json (a test enforces that).
  static CostConfig nse_delivery_defaults() { return CostConfig{}; }
  // Every key optional (default used); unknown keys / wrong types / negative rates -> ConfigError.
  static CostConfig from_json(const nlohmann::json& j);
  static CostConfig from_json_string(std::string_view text);
};

// notional is price*qty in paise (>= 0).
FeeBreakdown compute_fees(const CostConfig& c, Side side, Paise notional);

// Slippage is ADVERSE (buys pay more, sells receive less) and applies to Market and Stop fills only.
// Limit fills are never slipped: the limit price is the worst price you accept.
struct SlippageConfig {
  enum class Mode { None, FixedBps, RangePct };
  Mode mode = Mode::None;
  double value = 0.0;  // FixedBps: basis points of the price. RangePct: percent of the bar's (high - low).

  static SlippageConfig from_json(const nlohmann::json& j);
};

// Non-negative slippage amount in paise.
Paise slippage_amount(const SlippageConfig& s, Paise base_price, Paise bar_range);

}  // namespace bt
