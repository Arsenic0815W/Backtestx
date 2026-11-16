#include "bt/report.hpp"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <stdexcept>

#include "bt/date.hpp"
#include "bt/money.hpp"
#include "bt/report_assets.hpp"
#include "bt/version.hpp"

namespace bt {
namespace {

using json = nlohmann::json;
namespace fs = std::filesystem;

const char* kDisclaimer = "Not investment advice. Past performance does not predict future results.";

// ------------------------------------------------------------------------------------------------ reading files
std::string read_file(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  if (!f) throw std::runtime_error("report: cannot read " + path);
  std::ostringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

json read_json(const std::string& path) {
  try {
    return json::parse(read_file(path));
  } catch (const json::parse_error& e) {
    throw std::runtime_error("report: " + path + " is not valid JSON: " + e.what());
  }
}

std::vector<std::string> split_csv_line(const std::string& line) {
  std::vector<std::string> out;
  std::string cur;
  bool quoted = false;
  for (std::size_t i = 0; i < line.size(); ++i) {
    const char c = line[i];
    if (quoted) {
      if (c == '"') {
        if (i + 1 < line.size() && line[i + 1] == '"') { cur += '"'; ++i; }
        else quoted = false;
      } else cur += c;
    } else if (c == '"') quoted = true;
    else if (c == ',') { out.push_back(cur); cur.clear(); }
    else cur += c;
  }
  out.push_back(cur);
  return out;
}

struct Csv {
  std::string path;
  std::vector<std::string> header;
  std::vector<std::vector<std::string>> rows;
  std::size_t col(const std::string& name) const {
    for (std::size_t i = 0; i < header.size(); ++i)
      if (header[i] == name) return i;
    throw std::runtime_error("report: " + path + " has no column '" + name + "'");
  }
};

Csv read_csv(const std::string& path) {
  Csv c;
  c.path = path;
  std::istringstream in(read_file(path));
  std::string line;
  bool first = true;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty()) continue;
    std::vector<std::string> cells = split_csv_line(line);
    if (first) { c.header = std::move(cells); first = false; continue; }
    if (cells.size() != c.header.size())
      throw std::runtime_error("report: " + path + ": a row has " + std::to_string(cells.size()) + " cells, expected " + std::to_string(c.header.size()));
    c.rows.push_back(std::move(cells));
  }
  if (first) throw std::runtime_error("report: " + path + " is empty");
  return c;
}

std::int64_t to_i64(const std::string& s, const std::string& path) {
  errno = 0;
  char* end = nullptr;
  const long long v = std::strtoll(s.c_str(), &end, 10);
  if (s.empty() || *end != '\0' || errno != 0) throw std::runtime_error("report: " + path + ": '" + s + "' is not an integer");
  return v;
}
double to_dbl(const std::string& s, const std::string& path) {
  errno = 0;
  char* end = nullptr;
  const double v = std::strtod(s.c_str(), &end);
  if (s.empty() || *end != '\0' || errno != 0 || !std::isfinite(v)) throw std::runtime_error("report: " + path + ": '" + s + "' is not a number");
  return v;
}
Date to_date(const std::string& s, const std::string& path) {
  const auto d = parse_date(s);
  if (!d) throw std::runtime_error("report: " + path + ": '" + s + "' is not a date");
  return *d;
}

json opt_json(const std::optional<double>& v) { return v ? json(*v) : json(nullptr); }

// ------------------------------------------------------------------------------------------------ path shortening
std::string base_name(const std::string& p) {
  const std::size_t k = p.find_last_of("/\\");
  return k == std::string::npos ? p : p.substr(k + 1);
}
// Keys that hold file-system paths in run.json: replaced by the last path component, wherever they occur.
void shorten_paths(json& j) {
  if (j.is_object()) {
    for (auto it = j.begin(); it != j.end(); ++it) {
      if ((it.key() == "path" || it.key() == "dir" || it.key() == "costs_source") && it.value().is_string()) {
        const std::string s = it.value().get<std::string>();
        if (s.find_first_of("/\\") != std::string::npos) it.value() = base_name(s);
      } else shorten_paths(it.value());
    }
  } else if (j.is_array()) {
    for (json& e : j) shorten_paths(e);
  }
}

// ------------------------------------------------------------------------------------------------ equity curve
std::vector<DailyRecord> read_curve(const std::string& dir) {
  const Csv c = read_csv(dir + "/equity.csv");
  const std::size_t id = c.col("date"), ic = c.col("cash_paise"), ip = c.col("positions_value_paise"), ie = c.col("equity_paise");
  std::vector<DailyRecord> out;
  out.reserve(c.rows.size());
  for (const auto& r : c.rows) {
    DailyRecord d;
    d.date = to_date(r[id], c.path);
    d.cash = to_i64(r[ic], c.path);
    d.positions_value = to_i64(r[ip], c.path);
    d.equity = to_i64(r[ie], c.path);
    out.push_back(d);
  }
  return out;
}

BenchmarkSeries read_benchmark(const std::string& dir, const std::string& name) {
  const Csv c = read_csv(dir + "/benchmark.csv");
  const std::size_t id = c.col("date"), iv = c.col("value");
  BenchmarkSeries b;
  b.name = name;
  for (const auto& r : c.rows) {
    b.dates.push_back(to_date(r[id], c.path));
    b.values.push_back(to_dbl(r[iv], c.path));
  }
  return b;
}

json dates_json(const std::vector<Date>& d) {
  json a = json::array();
  for (Date x : d) a.push_back(format_date(x));
  return a;
}
json opt_array(const std::vector<std::optional<double>>& v) {
  json a = json::array();
  for (const auto& x : v) a.push_back(opt_json(x));
  return a;
}

}  // namespace

// ------------------------------------------------------------------------------------------------ public helpers
std::vector<MonthlyReturn> monthly_returns(const std::vector<DailyRecord>& c) {
  std::vector<MonthlyReturn> out;
  if (c.empty()) return out;
  Paise base = c.front().equity;
  Date base_date = c.front().date;
  std::size_t i = 0;
  while (i < c.size()) {
    const CivilDate cd = civil_from_days(c[i].date);
    std::size_t j = i;
    while (j + 1 < c.size()) {
      const CivilDate n = civil_from_days(c[j + 1].date);
      if (n.year != cd.year || n.month != cd.month) break;
      ++j;
    }
    MonthlyReturn m;
    m.year = cd.year;
    m.month = cd.month;
    m.from = base_date;
    m.to = c[j].date;
    if (base > 0 && m.from != m.to) m.ret = static_cast<double>(c[j].equity) / static_cast<double>(base) - 1.0;
    base = c[j].equity;
    base_date = c[j].date;
    out.push_back(m);
    i = j + 1;
  }
  out.front().first = true;
  out.back().last = true;
  return out;
}

std::vector<YearlyReturn> yearly_returns(const std::vector<DailyRecord>& c) {
  std::vector<YearlyReturn> out;
  if (c.empty()) return out;
  Paise base = c.front().equity;
  Date base_date = c.front().date;
  std::size_t i = 0;
  while (i < c.size()) {
    const int year = civil_from_days(c[i].date).year;
    std::size_t j = i;
    while (j + 1 < c.size() && civil_from_days(c[j + 1].date).year == year) ++j;
    YearlyReturn y;
    y.year = year;
    y.from = base_date;
    y.to = c[j].date;
    if (base > 0 && y.from != y.to) y.ret = static_cast<double>(c[j].equity) / static_cast<double>(base) - 1.0;
    base = c[j].equity;
    base_date = c[j].date;
    out.push_back(y);
    i = j + 1;
  }
  return out;
}

std::vector<std::optional<double>> drawdown_series(const std::vector<Paise>& equity) {
  std::vector<std::optional<double>> out;
  out.reserve(equity.size());
  Paise peak = 0;
  bool any = false;
  for (Paise e : equity) {
    if (!any || e > peak) { peak = e; any = true; }
    if (peak > 0) out.push_back(static_cast<double>(e) / static_cast<double>(peak) - 1.0);
    else out.push_back(std::nullopt);
  }
  return out;
}

std::optional<RebasedBenchmark> rebase_benchmark(const std::vector<DailyRecord>& curve, const BenchmarkSeries& b) {
  RebasedBenchmark out;
  std::size_t i = 0, j = 0;
  bool have = false;
  double v0 = 0;
  Paise e0 = 0;
  while (i < curve.size() && j < b.dates.size()) {
    if (curve[i].date < b.dates[j]) ++i;
    else if (curve[i].date > b.dates[j]) ++j;
    else {
      if (!have) { have = true; v0 = b.values[j]; e0 = curve[i].equity; }
      out.dates.push_back(curve[i].date);
      out.equity.push_back(static_cast<Paise>(std::llround(static_cast<double>(e0) * b.values[j] / v0)));
      ++i;
      ++j;
    }
  }
  if (!have) return std::nullopt;
  return out;
}

std::string script_safe_json(const json& j) {
  const std::string raw = j.dump(-1, ' ', true, json::error_handler_t::replace);
  std::string out;
  out.reserve(raw.size() + raw.size() / 16);
  for (char c : raw) {
    if (c == '<') out += "\\u003c";
    else out += c;
  }
  return out;
}

std::string html_escape(const std::string& s) {
  std::string o;
  for (char c : s) {
    switch (c) {
      case '&': o += "&amp;"; break;
      case '<': o += "&lt;"; break;
      case '>': o += "&gt;"; break;
      case '"': o += "&quot;"; break;
      case '\'': o += "&#39;"; break;
      default: o += c;
    }
  }
  return o;
}

bool is_sweep_dir(const std::string& dir) { return fs::exists(fs::path(dir) / "sweep_summary.json"); }

// ------------------------------------------------------------------------------------------------ run report data
json build_run_report_data(const std::string& dir) {
  json summary = read_json(dir + "/summary.json");
  json run = read_json(dir + "/run.json");
  const std::vector<DailyRecord> curve = read_curve(dir);
  if (curve.empty()) throw std::runtime_error("report: " + dir + "/equity.csv has no rows");
  if (summary.at("equity").at("n_records").get<std::int64_t>() != static_cast<std::int64_t>(curve.size()))
    throw std::runtime_error("report: summary.json and equity.csv disagree on the number of records (stale export?)");
  shorten_paths(run);

  json doc;
  doc["schema"] = 1;
  doc["kind"] = "run";
  doc["title"] = run.at("config").at("name");
  doc["run_tool"] = std::string("backtestx ") + version();
  doc["disclaimer"] = kDisclaimer;
  doc["run"] = run;
  doc["summary"] = summary;

  std::vector<Paise> eq;
  std::vector<Date> dates;
  for (const DailyRecord& r : curve) { eq.push_back(r.equity); dates.push_back(r.date); }
  doc["equity"] = {{"dates", dates_json(dates)}, {"equity_paise", eq}, {"drawdown", opt_array(drawdown_series(eq))}};

  // ---- benchmark rebased onto the strategy curve, measured with the same analytics code ----
  doc["benchmark"] = nullptr;
  if (fs::exists(dir + "/benchmark.csv")) {
    std::string name = "benchmark";
    if (summary.contains("benchmark") && summary["benchmark"].is_object() && summary["benchmark"].contains("name"))
      name = summary["benchmark"]["name"].get<std::string>();
    const BenchmarkSeries b = read_benchmark(dir, name);
    if (auto rb = rebase_benchmark(curve, b)) {
      AnalyticsConfig ac;
      ac.risk_free_annual = summary.at("config").at("risk_free_annual").get<double>();
      ac.trading_days = summary.at("config").at("trading_days_per_year").get<int>();
      std::vector<DailyRecord> bc;
      for (std::size_t i = 0; i < rb->dates.size(); ++i) {
        DailyRecord d;
        d.date = rb->dates[i];
        d.positions_value = rb->equity[i];
        d.equity = rb->equity[i];
        bc.push_back(d);
      }
      const Analytics ba = analyze(bc, {}, {}, ac, nullptr);
      doc["benchmark"] = {{"name", name},
                          {"dates", dates_json(rb->dates)},
                          {"equity_paise", rb->equity},
                          {"drawdown", opt_array(drawdown_series(rb->equity))},
                          {"metrics", to_json(ba)["equity"]}};
    }
  }

  // ---- monthly / yearly ----
  json mon = json::array(), yrs = json::array();
  for (const MonthlyReturn& m : monthly_returns(curve))
    mon.push_back({{"y", m.year}, {"m", m.month}, {"r", opt_json(m.ret)}, {"from", format_date(m.from)}, {"to", format_date(m.to)}, {"first", m.first}, {"last", m.last}});
  for (const YearlyReturn& y : yearly_returns(curve))
    yrs.push_back({{"y", y.year}, {"r", opt_json(y.ret)}, {"from", format_date(y.from)}, {"to", format_date(y.to)}});
  doc["monthly"] = mon;
  doc["yearly"] = yrs;

  // ---- trades ----
  {
    const Csv t = read_csv(dir + "/trades.csv");
    const std::vector<std::string> keep = {"symbol", "direction", "entry_date", "exit_date", "qty", "entry_value_paise", "exit_value_paise",
                                           "gross_pnl_paise", "fees_paise", "net_pnl_paise"};
    std::vector<std::size_t> idx;
    for (const std::string& k : keep) idx.push_back(t.col(k));
    json cols = json::array();
    for (const std::string& k : keep) cols.push_back(k);
    cols.push_back("return");
    json rows = json::array();
    const std::size_t i_entry = t.col("entry_value_paise"), i_net = t.col("net_pnl_paise");
    for (const auto& r : t.rows) {
      json row = json::array();
      for (std::size_t k = 0; k < keep.size(); ++k) {
        if (k < 4) row.push_back(r[idx[k]]);  // symbol, direction, dates stay strings
        else row.push_back(to_i64(r[idx[k]], t.path));
      }
      const std::int64_t entry = to_i64(r[i_entry], t.path), net = to_i64(r[i_net], t.path);
      row.push_back(entry != 0 ? json(static_cast<double>(net) / static_cast<double>(entry)) : json(nullptr));
      rows.push_back(row);
    }
    doc["trades"] = {{"columns", cols}, {"rows", rows}};
    // P&L split: closed round trips (net of fees) + everything else (open positions marked at the last close, and the entry fees
    // already paid on them) = the total change in equity.
    std::int64_t closed_net = 0;
    for (const auto& r : t.rows) closed_net += to_i64(r[i_net], t.path);
    const std::int64_t total = curve.back().equity - curve.front().equity;
    if (summary.at("trades").at("net_pnl_paise").is_number() && summary.at("trades").at("net_pnl_paise").get<std::int64_t>() != total)
      throw std::runtime_error("report: summary.json net_pnl_paise disagrees with equity.csv (stale export?)");
    doc["pnl"] = {{"closed_trades_net_paise", closed_net}, {"unrealised_paise", total - closed_net}, {"total_paise", total}};
  }

  // ---- fees and open positions ----
  {
    const Csv f = read_csv(dir + "/fills.csv");
    const char* comp[] = {"brokerage_paise", "stt_paise", "exchange_paise", "sebi_paise", "stamp_paise", "gst_paise"};
    const char* label[] = {"Brokerage", "STT", "Exchange charges", "SEBI fee", "Stamp duty", "GST"};
    std::int64_t totals[6] = {0, 0, 0, 0, 0, 0}, fees_sum = 0, buy = 0, sell = 0;
    struct Sym { std::int64_t fills = 0, notional = 0, fees = 0; };
    std::map<std::string, Sym> by_symbol;
    std::map<std::string, std::int64_t> position;
    const std::size_t i_sym = f.col("symbol"), i_side = f.col("side"), i_fees = f.col("fees_paise"), i_not = f.col("notional_paise"), i_pos = f.col("position_after");
    std::size_t ci[6];
    for (int k = 0; k < 6; ++k) ci[k] = f.col(comp[k]);
    for (const auto& r : f.rows) {
      std::int64_t row_sum = 0;
      for (int k = 0; k < 6; ++k) { const std::int64_t v = to_i64(r[ci[k]], f.path); totals[k] += v; row_sum += v; }
      const std::int64_t fee = to_i64(r[i_fees], f.path);
      if (fee != row_sum) throw std::runtime_error("report: " + f.path + ": a fill's fee components do not add up to fees_paise");
      fees_sum += fee;
      (r[i_side] == "buy" ? buy : sell) += fee;
      Sym& s = by_symbol[r[i_sym]];
      s.fills += 1;
      s.notional += to_i64(r[i_not], f.path);
      s.fees += fee;
      position[r[i_sym]] = to_i64(r[i_pos], f.path);
    }
    if (fees_sum != summary.at("trades").at("total_fees_paise").get<std::int64_t>())
      throw std::runtime_error("report: the fills.csv fee total disagrees with summary.json total_fees_paise (stale export?)");
    json comps = json::array();
    for (int k = 0; k < 6; ++k)
      comps.push_back({{"label", label[k]}, {"key", comp[k]}, {"paise", totals[k]},
                       {"share", fees_sum != 0 ? json(static_cast<double>(totals[k]) / static_cast<double>(fees_sum)) : json(nullptr)}});
    json syms = json::array();
    for (const auto& kv : by_symbol) syms.push_back({{"symbol", kv.first}, {"fills", kv.second.fills}, {"notional_paise", kv.second.notional}, {"paise", kv.second.fees}});
    doc["fees"] = {{"total_paise", fees_sum}, {"components", comps}, {"by_symbol", syms}, {"by_side", {{"buy_paise", buy}, {"sell_paise", sell}}}};
    json open = json::array();
    for (const auto& kv : position)
      if (kv.second != 0) open.push_back({{"symbol", kv.first}, {"qty", kv.second}});
    doc["open_positions"] = open;
  }

  // ---- context: this folder is the best/ run of a sweep ----
  const fs::path p = fs::absolute(fs::path(dir)).lexically_normal();
  fs::path leaf = p.filename();
  if (leaf.empty()) leaf = p.parent_path().filename();
  const fs::path parent = (p.filename().empty() ? p.parent_path() : p).parent_path();
  if (leaf == "best" && fs::exists(parent / "sweep_summary.json")) {
    const json ss = read_json((parent / "sweep_summary.json").string());
    const bool split = ss.at("windows").at("mode") == "single_split";
    json ctx = {{"link", "../report.html"}, {"link_text", "\xE2\x86\x90 Back to the sweep report"}};
    if (split) {
      const json& oos = ss.at("windows").at("out_of_sample");
      ctx["title"] = "Out-of-sample run.";
      ctx["text"] = "These parameters were chosen on the in-sample window; this report is the run on the out-of-sample window " + oos.at("start").get<std::string>() + " to " +
                    oos.at("end").get<std::string>() + ", started with fresh capital. It is not a continuation of the in-sample result.";
    } else {
      ctx["title"] = "Selected parameters, whole window.";
      ctx["text"] = "No out-of-sample split was configured: these parameters were chosen as the best of " + std::to_string(ss.at("grid").at("n_combinations").get<std::int64_t>()) +
                    " on the same data they are reported on, so the result is optimistically biased.";
    }
    doc["context"] = ctx;
  }
  return doc;
}

// ------------------------------------------------------------------------------------------------ sweep report data
json build_sweep_report_data(const std::string& dir) {
  const json ss = read_json(dir + "/sweep_summary.json");
  const Csv c = read_csv(dir + "/sweep.csv");
  json doc;
  doc["schema"] = 1;
  doc["kind"] = "sweep";
  doc["title"] = ss.at("name");
  doc["run_tool"] = std::string("backtestx ") + version();
  doc["disclaimer"] = kDisclaimer;
  doc["summary"] = ss;

  // axes = the columns between run_id and status, typed from the grid in sweep_summary.json
  const std::size_t i_status = c.col("status");
  if (c.header.empty() || c.header[0] != "run_id") throw std::runtime_error("report: " + c.path + ": first column must be run_id");
  std::vector<std::string> axes(c.header.begin() + 1, c.header.begin() + static_cast<std::ptrdiff_t>(i_status));
  std::vector<bool> axis_is_string;
  for (const std::string& a : axes) {
    bool str = false;
    for (const json& v : ss.at("grid").at("axes").at(a))
      if (v.is_string()) str = true;
    axis_is_string.push_back(str);
  }
  doc["axes"] = axes;
  doc["columns"] = c.header;
  json rows = json::array();
  for (const auto& r : c.rows) {
    json row = json::array();
    for (std::size_t k = 0; k < r.size(); ++k) {
      const std::string& h = c.header[k];
      if (k >= 1 && k <= axes.size()) row.push_back(axis_is_string[k - 1] ? json(r[k]) : json(to_dbl(r[k], c.path)));
      else if (h == "run_id" || h == "selected") row.push_back(to_i64(r[k], c.path));
      else if (h == "status") row.push_back(r[k]);
      else if (h == "message" || h == "run_hash") row.push_back(r[k].empty() ? json(nullptr) : json(r[k]));
      else row.push_back(r[k].empty() ? json(nullptr) : json(to_dbl(r[k], c.path)));
    }
    rows.push_back(row);
  }
  doc["rows"] = rows;
  if (fs::exists(dir + "/best/report.html")) doc["best_report"] = "best/report.html";
  return doc;
}

// ------------------------------------------------------------------------------------------------ rendering
std::string render_report_html(const json& data) {
  const std::string tpl = report_assets::report_template();
  const std::string css = report_assets::report_css();
  const std::string js = report_assets::report_js();
  const std::string title = html_escape(data.at("title").get<std::string>()) + " - BacktestX";
  const std::string payload = script_safe_json(data);

  // Fill the markers by scanning the TEMPLATE once, left to right: inserted text is never re-scanned, so a config name that
  // happens to contain a marker cannot be substituted.
  const struct { const char* marker; const std::string* value; } parts[] = {{"@@BT_TITLE@@", &title}, {"@@BT_CSS@@", &css}, {"@@BT_DATA@@", &payload}, {"@@BT_JS@@", &js}};
  std::string out;
  std::size_t pos = 0;
  for (const auto& p : parts) {
    const std::size_t k = tpl.find(p.marker, pos);
    if (k == std::string::npos) throw std::runtime_error(std::string("report template is missing ") + p.marker);
    out.append(tpl, pos, k - pos);
    out += *p.value;
    pos = k + std::string(p.marker).size();
  }
  out.append(tpl, pos, std::string::npos);
  return out;
}

namespace {
void write_html(const std::string& dir, const json& data) {
  const std::string html = render_report_html(data);
  const std::string path = dir + "/report.html";
  std::ofstream f(path, std::ios::binary);
  if (!f) throw std::runtime_error("report: cannot write " + path);
  f << html;
  f.close();
  if (!f) throw std::runtime_error("report: failed while writing " + path);
}
}  // namespace

void write_run_report(const std::string& dir) { write_html(dir, build_run_report_data(dir)); }

void write_sweep_reports(const std::string& dir) {
  if (fs::exists(dir + "/best/summary.json")) write_run_report(dir + "/best");
  write_html(dir, build_sweep_report_data(dir));
}

}  // namespace bt
