#include "bt/costs.hpp"

#include <algorithm>

#include "bt/json_util.hpp"

namespace bt {

CostConfig CostConfig::from_json(const nlohmann::json& j) {
  const std::string root = "costs";
  cfg::check_keys(j, {"meta", "brokerage", "stt_buy_pct", "stt_sell_pct", "exchange_txn_pct", "sebi_pct",
                      "stamp_buy_pct", "gst_pct"},
                  root);
  CostConfig c;

  if (const auto m = j.find("meta"); m != j.end()) {  // documentation only, but type-checked
    cfg::check_keys(*m, {"as_of", "status", "sources", "notes"}, "costs.meta");
    cfg::get_str(*m, "as_of", "", "costs.meta");
    cfg::get_str(*m, "status", "", "costs.meta");
    cfg::get_str(*m, "notes", "", "costs.meta");
    if (const auto s = m->find("sources"); s != m->end()) {
      if (!s->is_array()) throw ConfigError("costs.meta.sources: expected an array of strings");
      for (const auto& e : *s)
        if (!e.is_string()) throw ConfigError("costs.meta.sources: expected an array of strings");
    }
  }

  if (const auto b = j.find("brokerage"); b != j.end()) {
    cfg::check_keys(*b, {"type", "flat_rupees", "pct", "min_rupees", "max_rupees"}, "costs.brokerage");
    const std::string t = cfg::get_str(*b, "type", "flat", "costs.brokerage");
    if (t == "flat") c.brokerage_type = BrokerageType::Flat;
    else if (t == "percent") c.brokerage_type = BrokerageType::Percent;
    else throw ConfigError("costs.brokerage.type: must be \"flat\" or \"percent\", got \"" + t + "\"");
    c.brokerage_flat_rupees = cfg::get_num(*b, "flat_rupees", c.brokerage_flat_rupees, "costs.brokerage");
    c.brokerage_pct = cfg::get_num(*b, "pct", c.brokerage_pct, "costs.brokerage", 0.0, 100.0);
    c.brokerage_min_rupees = cfg::get_num(*b, "min_rupees", c.brokerage_min_rupees, "costs.brokerage");
    c.brokerage_max_rupees = cfg::get_num(*b, "max_rupees", c.brokerage_max_rupees, "costs.brokerage");
    if (c.brokerage_max_rupees > 0 && c.brokerage_min_rupees > c.brokerage_max_rupees)
      throw ConfigError("costs.brokerage: min_rupees exceeds max_rupees");
  }
  c.stt_buy_pct = cfg::get_num(j, "stt_buy_pct", c.stt_buy_pct, root, 0.0, 100.0);
  c.stt_sell_pct = cfg::get_num(j, "stt_sell_pct", c.stt_sell_pct, root, 0.0, 100.0);
  c.exchange_txn_pct = cfg::get_num(j, "exchange_txn_pct", c.exchange_txn_pct, root, 0.0, 100.0);
  c.sebi_pct = cfg::get_num(j, "sebi_pct", c.sebi_pct, root, 0.0, 100.0);
  c.stamp_buy_pct = cfg::get_num(j, "stamp_buy_pct", c.stamp_buy_pct, root, 0.0, 100.0);
  c.gst_pct = cfg::get_num(j, "gst_pct", c.gst_pct, root, 0.0, 100.0);
  return c;
}

CostConfig CostConfig::from_json_string(std::string_view text) {
  nlohmann::json j;
  try {
    j = nlohmann::json::parse(text);
  } catch (const nlohmann::json::parse_error& e) {
    throw ConfigError(std::string("costs: invalid JSON: ") + e.what());
  }
  return from_json(j);
}

FeeBreakdown compute_fees(const CostConfig& c, Side side, Paise notional) {
  FeeBreakdown f;
  if (c.brokerage_type == BrokerageType::Flat) {
    f.brokerage = to_paise(c.brokerage_flat_rupees);
  } else {
    Paise b = apply_rate_ppb(notional, rate_pct_to_ppb(c.brokerage_pct));
    if (c.brokerage_min_rupees > 0) b = std::max(b, to_paise(c.brokerage_min_rupees));
    if (c.brokerage_max_rupees > 0) b = std::min(b, to_paise(c.brokerage_max_rupees));
    f.brokerage = b;
  }
  f.stt = apply_rate_ppb(notional, rate_pct_to_ppb(side == Side::Buy ? c.stt_buy_pct : c.stt_sell_pct));
  f.exchange = apply_rate_ppb(notional, rate_pct_to_ppb(c.exchange_txn_pct));
  f.sebi = apply_rate_ppb(notional, rate_pct_to_ppb(c.sebi_pct));
  f.stamp = side == Side::Buy ? apply_rate_ppb(notional, rate_pct_to_ppb(c.stamp_buy_pct)) : 0;
  f.gst = apply_rate_ppb(f.brokerage + f.exchange + f.sebi, rate_pct_to_ppb(c.gst_pct));
  return f;
}

SlippageConfig SlippageConfig::from_json(const nlohmann::json& j) {
  cfg::check_keys(j, {"mode", "value"}, "slippage");
  SlippageConfig s;
  const std::string m = cfg::get_str(j, "mode", "none", "slippage");
  if (m == "none") s.mode = Mode::None;
  else if (m == "fixed_bps") s.mode = Mode::FixedBps;
  else if (m == "range_pct") s.mode = Mode::RangePct;
  else throw ConfigError("slippage.mode: must be \"none\", \"fixed_bps\" or \"range_pct\", got \"" + m + "\"");
  s.value = cfg::get_num(j, "value", 0.0, "slippage", 0.0, s.mode == Mode::RangePct ? 100.0 : 10000.0);
  return s;
}

Paise slippage_amount(const SlippageConfig& s, Paise base_price, Paise bar_range) {
  switch (s.mode) {
    case SlippageConfig::Mode::None: return 0;
    case SlippageConfig::Mode::FixedBps: return apply_rate_ppb(base_price, rate_bps_to_ppb(s.value));
    case SlippageConfig::Mode::RangePct: return apply_rate_ppb(bar_range, rate_pct_to_ppb(s.value));
  }
  return 0;
}

}  // namespace bt
