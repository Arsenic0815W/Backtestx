#include "bt/run_config.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

#include "bt/strategies.hpp"

namespace bt {
namespace {

namespace fs = std::filesystem;
using json = nlohmann::json;

std::string join_path(const std::string& base, const std::string& p) {
  const fs::path q(p);
  if (q.is_absolute()) return q.lexically_normal().generic_string();
  return (fs::path(base) / q).lexically_normal().generic_string();
}

Date date_value(const json& v, const std::string& path) {
  if (!v.is_string()) throw ConfigError(path + ": expected a date string \"YYYY-MM-DD\", got " + cfg::type_name(v));
  const auto d = parse_date(v.get<std::string>());
  if (!d) throw ConfigError(path + ": \"" + v.get<std::string>() + "\" is not a valid YYYY-MM-DD date");
  return *d;
}

std::optional<Date> opt_date(const json& j, const char* key, const std::string& path) {
  const auto it = j.find(key);
  if (it == j.end()) return std::nullopt;
  return date_value(*it, cfg::join(path, key));
}

}  // namespace

std::string symbol_file_name(const std::string& symbol) {
  std::size_t i = 0;
  while (i < symbol.size() && symbol[i] == '^') ++i;
  return symbol.substr(i) + ".csv";
}

RunConfig parse_run_config(const json& j, const std::string& base_dir, const std::string& default_name, json* sweep_block) {
  cfg::check_keys(j, {"name", "data", "period", "capital", "costs", "costs_file", "slippage", "execution", "sizing", "strategy",
                      "benchmark", "analytics", "sweep"}, "");
  RunConfig c;
  c.base_dir = base_dir;
  c.name = cfg::get_str(j, "name", default_name, "");

  // ---- sweep block ----
  if (const auto it = j.find("sweep"); it != j.end()) {
    if (!sweep_block) throw ConfigError("sweep: this config has a \"sweep\" block; run it with `backtestx sweep` (a plain run does not accept one)");
    *sweep_block = *it;
  }

  // ---- data ----
  const auto dit = j.find("data");
  if (dit == j.end()) throw ConfigError("data: required");
  cfg::check_keys(*dit, {"dir", "files", "prices", "null_rows", "max_daily_move", "exclude_dates"}, "data");
  c.data_dir = join_path(base_dir, cfg::get_str(*dit, "dir", "../data", "data"));
  const std::string prices = cfg::get_str(*dit, "prices", "adjusted", "data");
  if (prices == "adjusted") c.prices = PriceBasis::Adjusted;
  else if (prices == "raw") c.prices = PriceBasis::Raw;
  else throw ConfigError("data.prices: must be \"adjusted\" or \"raw\", got \"" + prices + "\"");
  const std::string nulls = cfg::get_str(*dit, "null_rows", "fail", "data");
  if (nulls == "fail") c.null_rows = NullPolicy::Fail;
  else if (nulls == "skip") c.null_rows = NullPolicy::Skip;
  else throw ConfigError("data.null_rows: must be \"fail\" or \"skip\", got \"" + nulls + "\"");
  c.max_daily_move = cfg::get_num(*dit, "max_daily_move", 0.5, "data", 0.0, 1e9);
  if (!(c.max_daily_move > 0.0)) throw ConfigError("data.max_daily_move: must be > 0");

  const auto fit = dit->find("files");
  if (fit == dit->end()) throw ConfigError("data.files: required");
  if (!fit->is_array() || fit->empty()) throw ConfigError("data.files: expected a non-empty array");
  std::set<Symbol> seen;
  for (std::size_t i = 0; i < fit->size(); ++i) {
    const json& e = (*fit)[i];
    const std::string path = "data.files[" + std::to_string(i) + "]";
    FileSpec f;
    if (e.is_string()) {
      f.symbol = e.get<std::string>();
      if (f.symbol.empty()) throw ConfigError(path + ": empty symbol");
      f.path = join_path(c.data_dir, symbol_file_name(f.symbol));
    } else if (e.is_object()) {
      cfg::check_keys(e, {"symbol", "path"}, path);
      f.symbol = cfg::get_str(e, "symbol", "", path);
      if (f.symbol.empty()) throw ConfigError(path + ".symbol: required");
      f.path = e.contains("path") ? join_path(base_dir, cfg::get_str(e, "path", "", path))
                                  : join_path(c.data_dir, symbol_file_name(f.symbol));
    } else {
      throw ConfigError(path + ": expected a symbol string or an object {symbol, path}, got " + cfg::type_name(e));
    }
    if (!seen.insert(f.symbol).second) throw ConfigError(path + ": symbol \"" + f.symbol + "\" is listed twice");
    c.files.push_back(std::move(f));
  }

  std::map<Symbol, std::vector<Date>> excl;
  if (const auto eit = dit->find("exclude_dates"); eit != dit->end()) {
    cfg::expect_object(*eit, "data.exclude_dates");
    for (auto it = eit->begin(); it != eit->end(); ++it) {
      const std::string path = "data.exclude_dates." + it.key();
      if (!it->is_array() || it->empty()) throw ConfigError(path + ": expected a non-empty array of dates");
      std::vector<Date>& v = excl[it.key()];
      for (std::size_t k = 0; k < it->size(); ++k) v.push_back(date_value((*it)[k], path + "[" + std::to_string(k) + "]"));
    }
  }

  // ---- period ----
  if (const auto it = j.find("period"); it != j.end()) {
    cfg::check_keys(*it, {"start", "end"}, "period");
    c.start = opt_date(*it, "start", "period");
    c.end = opt_date(*it, "end", "period");
    if (c.start && c.end && *c.start > *c.end) throw ConfigError("period: start (" + format_date(*c.start) + ") is after end (" + format_date(*c.end) + ")");
  }

  // ---- account ----
  c.capital_rupees = cfg::get_num(j, "capital", 1'000'000.0, "", 0.0, 1e12);
  if (!(c.capital_rupees > 0.0)) throw ConfigError("capital: must be > 0");
  if (j.contains("costs") && j.contains("costs_file")) throw ConfigError("costs / costs_file: give one of them, not both");
  if (const auto it = j.find("costs"); it != j.end()) {
    c.costs = CostConfig::from_json(*it);
    c.costs_source = "inline \"costs\" block";
  }
  if (const auto it = j.find("costs_file"); it != j.end()) {
    if (!it->is_string()) throw ConfigError("costs_file: expected a path string");
    const std::string p = join_path(base_dir, it->get<std::string>());
    std::ifstream in(p, std::ios::binary);
    if (!in) throw ConfigError("costs_file: cannot open " + p);
    std::stringstream ss;
    ss << in.rdbuf();
    try {
      c.costs = CostConfig::from_json_string(ss.str());
    } catch (const ConfigError& e) {
      throw ConfigError("costs_file " + p + ": " + e.what());
    }
    c.costs_source = p;
  }
  if (const auto it = j.find("slippage"); it != j.end()) c.slippage = SlippageConfig::from_json(*it);
  if (const auto it = j.find("execution"); it != j.end()) {
    cfg::check_keys(*it, {"allow_short", "max_volume_participation"}, "execution");
    c.allow_short = cfg::get_bool(*it, "allow_short", false, "execution");
    c.max_volume_participation = cfg::get_num(*it, "max_volume_participation", 0.0, "execution", 0.0, 1.0);
  }
  if (const auto it = j.find("sizing"); it != j.end()) {
    cfg::check_keys(*it, {"allocation"}, "sizing");
    if (it->contains("allocation")) {
      const double a = cfg::get_num(*it, "allocation", 0.95, "sizing", 0.0, 1.0);
      if (!(a > 0.0)) throw ConfigError("sizing.allocation: must be > 0 and <= 1");
      c.allocation = a;
    }
  }

  // ---- strategy ----
  const auto sit = j.find("strategy");
  if (sit == j.end()) throw ConfigError("strategy: required");
  cfg::check_keys(*sit, {"name", "params"}, "strategy");
  c.strategy = cfg::get_str(*sit, "name", "", "strategy");
  if (c.strategy.empty()) throw ConfigError("strategy.name: required");
  if (const auto pit = sit->find("params"); pit != sit->end()) {
    cfg::expect_object(*pit, "strategy.params");
    c.params = *pit;
  }
  if (c.params.contains("allocation"))
    throw ConfigError("strategy.params.allocation: allocation belongs in the top-level \"sizing\": {\"allocation\": ...} block, not in the strategy params");
  {  // fail early (before any data is read) on a bad strategy name or bad parameters
    json p = c.params;
    if (c.allocation) p["allocation"] = *c.allocation;
    make_strategy(c.strategy, p);
  }

  // ---- benchmark ----
  if (const auto it = j.find("benchmark"); it != j.end()) {
    if (it->is_null()) {
      c.benchmark.mode = BenchmarkSpec::Mode::None;
    } else if (it->is_string()) {
      c.benchmark.mode = BenchmarkSpec::Mode::Explicit;
      c.benchmark.symbol = it->get<std::string>();
      if (c.benchmark.symbol.empty()) throw ConfigError("benchmark: empty symbol");
    } else if (it->is_object()) {
      cfg::check_keys(*it, {"symbol", "path", "max_daily_move"}, "benchmark");
      c.benchmark.mode = BenchmarkSpec::Mode::Explicit;
      c.benchmark.symbol = cfg::get_str(*it, "symbol", "", "benchmark");
      if (c.benchmark.symbol.empty()) throw ConfigError("benchmark.symbol: required");
      c.benchmark.max_daily_move = cfg::get_num(*it, "max_daily_move", 0.15, "benchmark", 0.0, 1e9);
      if (!(c.benchmark.max_daily_move > 0.0)) throw ConfigError("benchmark.max_daily_move: must be > 0");
      if (it->contains("path")) c.benchmark.path = join_path(base_dir, cfg::get_str(*it, "path", "", "benchmark"));
    } else {
      throw ConfigError(std::string("benchmark: expected a symbol string, an object, or null, got ") + cfg::type_name(*it));
    }
  }
  if (c.benchmark.mode != BenchmarkSpec::Mode::None && c.benchmark.path.empty())
    c.benchmark.path = join_path(c.data_dir, symbol_file_name(c.benchmark.symbol));

  // ---- analytics ----
  if (const auto it = j.find("analytics"); it != j.end()) {
    cfg::check_keys(*it, {"risk_free_annual", "trading_days"}, "analytics");
    c.analytics.risk_free_annual = cfg::get_num(*it, "risk_free_annual", c.analytics.risk_free_annual, "analytics", -0.5, 1.0);
    c.analytics.trading_days = static_cast<int>(cfg::get_int(*it, "trading_days", c.analytics.trading_days, "analytics", 1, 366));
  }

  // ---- exclusions must refer to a loaded symbol (typo protection) ----
  for (auto& [sym, dates] : excl) {
    const bool is_traded = seen.count(sym) > 0;
    const bool is_bench = c.benchmark.mode != BenchmarkSpec::Mode::None && sym == c.benchmark.symbol;
    if (!is_traded && !is_bench)
      throw ConfigError("data.exclude_dates." + sym + ": no such symbol in data.files or benchmark");
    std::sort(dates.begin(), dates.end());
    c.exclude_dates.emplace_back(sym, dates);
  }
  return c;
}

RunConfig load_run_config(const std::string& file, json* sweep_block) {
  std::ifstream in(file, std::ios::binary);
  if (!in) throw ConfigError(file + ": cannot open config file");
  std::stringstream ss;
  ss << in.rdbuf();
  json j;
  try {
    j = json::parse(ss.str());
  } catch (const json::parse_error& e) {
    throw ConfigError(file + ": invalid JSON: " + e.what());
  }
  const fs::path p(file);
  const std::string dir = p.has_parent_path() ? p.parent_path().string() : ".";
  try {
    return parse_run_config(j, dir, p.stem().string(), sweep_block);
  } catch (const ConfigError& e) {
    throw ConfigError(file + ": " + e.what());
  }
}

json costs_to_json(const CostConfig& c) {
  return {{"brokerage", {{"type", c.brokerage_type == BrokerageType::Flat ? "flat" : "percent"},
                         {"flat_rupees", c.brokerage_flat_rupees},
                         {"pct", c.brokerage_pct},
                         {"min_rupees", c.brokerage_min_rupees},
                         {"max_rupees", c.brokerage_max_rupees}}},
          {"stt_buy_pct", c.stt_buy_pct},
          {"stt_sell_pct", c.stt_sell_pct},
          {"exchange_txn_pct", c.exchange_txn_pct},
          {"sebi_pct", c.sebi_pct},
          {"stamp_buy_pct", c.stamp_buy_pct},
          {"gst_pct", c.gst_pct}};
}

json to_json(const RunConfig& c) {
  json files = json::array();
  for (const FileSpec& f : c.files) files.push_back({{"symbol", f.symbol}, {"path", f.path}});
  json excl = json::object();
  for (const auto& [sym, dates] : c.exclude_dates) {
    json a = json::array();
    for (Date d : dates) a.push_back(format_date(d));
    excl[sym] = a;
  }
  json p = c.params;
  if (c.allocation) p["allocation"] = *c.allocation;
  json bench;
  switch (c.benchmark.mode) {
    case BenchmarkSpec::Mode::None: bench = nullptr; break;
    default:
      bench = {{"mode", c.benchmark.mode == BenchmarkSpec::Mode::Default ? "default" : "explicit"},
               {"symbol", c.benchmark.symbol},
               {"path", c.benchmark.path},
               {"max_daily_move", c.benchmark.max_daily_move}};
  }
  const char* slip = c.slippage.mode == SlippageConfig::Mode::None ? "none"
                     : c.slippage.mode == SlippageConfig::Mode::FixedBps ? "fixed_bps" : "range_pct";
  return {{"name", c.name},
          {"data",
           {{"dir", c.data_dir},
            {"files", files},
            {"prices", c.prices == PriceBasis::Adjusted ? "adjusted" : "raw"},
            {"null_rows", c.null_rows == NullPolicy::Fail ? "fail" : "skip"},
            {"max_daily_move", c.max_daily_move},
            {"exclude_dates", excl}}},
          {"period", {{"start", c.start ? json(format_date(*c.start)) : json(nullptr)},
                      {"end", c.end ? json(format_date(*c.end)) : json(nullptr)}}},
          {"capital", c.capital_rupees},
          {"costs", costs_to_json(c.costs)},
          {"costs_source", c.costs_source},
          {"slippage", {{"mode", slip}, {"value", c.slippage.value}}},
          {"execution", {{"allow_short", c.allow_short}, {"max_volume_participation", c.max_volume_participation}}},
          {"strategy", {{"name", c.strategy}, {"params", p}}},
          {"benchmark", bench},
          {"analytics", {{"risk_free_annual", c.analytics.risk_free_annual}, {"trading_days", c.analytics.trading_days}}}};
}

}  // namespace bt
