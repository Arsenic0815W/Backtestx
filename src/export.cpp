#include "bt/export.hpp"

#include <charconv>
#include <filesystem>
#include <functional>
#include <fstream>
#include <ostream>
#include <stdexcept>

namespace bt {
namespace {

std::string dbl(double v) {
  char buf[64];
  const auto r = std::to_chars(buf, buf + sizeof buf, v);
  return std::string(buf, r.ptr);
}

void write_file(const std::string& path, const std::function<void(std::ostream&)>& fn) {
  std::ofstream f(path, std::ios::binary);  // binary: keep LF on Windows
  if (!f) throw std::runtime_error("cannot write " + path);
  fn(f);
  f.flush();
  if (!f) throw std::runtime_error("write failed for " + path);
}

}  // namespace

void write_equity_csv(std::ostream& out, const std::vector<DailyRecord>& curve) {
  out << "date,cash_paise,positions_value_paise,equity_paise\n";
  for (const DailyRecord& r : curve)
    out << format_date(r.date) << ',' << r.cash << ',' << r.positions_value << ',' << r.equity << '\n';
}

void write_trades_csv(std::ostream& out, const std::vector<Trade>& trades, const std::vector<Symbol>& symbols) {
  out << "symbol,direction,entry_date,exit_date,entry_day,exit_day,qty,entry_value_paise,exit_value_paise,"
         "gross_pnl_paise,fees_paise,net_pnl_paise,num_fills\n";
  for (const Trade& t : trades)
    out << symbols.at(t.symbol) << ',' << (t.direction == Side::Buy ? "long" : "short") << ',' << format_date(t.entry_date)
        << ',' << format_date(t.exit_date) << ',' << t.entry_day << ',' << t.exit_day << ',' << t.qty << ','
        << t.entry_value << ',' << t.exit_value << ',' << t.gross_pnl << ',' << t.fees.total() << ',' << t.net_pnl << ','
        << t.num_fills << '\n';
}

void write_fills_csv(std::ostream& out, const std::vector<Fill>& fills, const std::vector<Symbol>& symbols) {
  out << "order_id,date,day_index,symbol,side,qty,price_paise,notional_paise,brokerage_paise,stt_paise,exchange_paise,"
         "sebi_paise,stamp_paise,gst_paise,fees_paise,cash_after_paise,position_after\n";
  for (const Fill& f : fills)
    out << f.order_id << ',' << format_date(f.date) << ',' << f.day_index << ',' << symbols.at(f.symbol) << ','
        << (f.side == Side::Buy ? "buy" : "sell") << ',' << f.qty << ',' << f.price << ',' << f.notional << ','
        << f.fees.brokerage << ',' << f.fees.stt << ',' << f.fees.exchange << ',' << f.fees.sebi << ',' << f.fees.stamp << ','
        << f.fees.gst << ',' << f.fees.total() << ',' << f.cash_after << ',' << f.position_after << '\n';
}

void write_benchmark_csv(std::ostream& out, const BenchmarkSeries& b) {
  out << "date,value\n";
  for (std::size_t i = 0; i < b.dates.size(); ++i) out << format_date(b.dates[i]) << ',' << dbl(b.values[i]) << '\n';
}

void write_summary_json(std::ostream& out, const Analytics& a) { out << to_json(a).dump(2) << '\n'; }

void export_run(const std::string& dir, const EngineResult& r, const Analytics& a, const BenchmarkSeries* benchmark) {
  std::filesystem::create_directories(dir);
  const std::string d = dir + "/";
  write_file(d + "equity.csv", [&](std::ostream& o) { write_equity_csv(o, r.equity_curve); });
  write_file(d + "trades.csv", [&](std::ostream& o) { write_trades_csv(o, r.trades, r.symbols); });
  write_file(d + "fills.csv", [&](std::ostream& o) { write_fills_csv(o, r.fills, r.symbols); });
  if (benchmark) write_file(d + "benchmark.csv", [&](std::ostream& o) { write_benchmark_csv(o, *benchmark); });
  write_file(d + "summary.json", [&](std::ostream& o) { write_summary_json(o, a); });
}

}  // namespace bt
