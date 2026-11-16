#pragma once
// CSV / JSON exporters for a finished run. These files are the ONLY input tools/ref_metrics.py uses, and they contain every
// field the analytics read. All money is exact integer paise.
//
//   equity.csv     date,cash_paise,positions_value_paise,equity_paise        (one row per equity-curve record)
//   trades.csv     symbol,direction,entry_date,exit_date,entry_day,exit_day,qty,entry_value_paise,exit_value_paise,
//                  gross_pnl_paise,fees_paise,net_pnl_paise,num_fills        (closed round trips; direction long|short)
//   fills.csv      order_id,date,day_index,symbol,side,qty,price_paise,notional_paise,brokerage_paise,stt_paise,
//                  exchange_paise,sebi_paise,stamp_paise,gst_paise,fees_paise,cash_after_paise,position_after
//   benchmark.csv  date,value                                                (doubles in shortest round-trip form)
//   summary.json   the analytics (config + equity + trades + benchmark)
#include <iosfwd>
#include <string>
#include <vector>

#include "bt/analytics.hpp"

namespace bt {

void write_equity_csv(std::ostream& out, const std::vector<DailyRecord>& curve);
void write_trades_csv(std::ostream& out, const std::vector<Trade>& trades, const std::vector<Symbol>& symbols);
void write_fills_csv(std::ostream& out, const std::vector<Fill>& fills, const std::vector<Symbol>& symbols);
void write_benchmark_csv(std::ostream& out, const BenchmarkSeries& b);
void write_summary_json(std::ostream& out, const Analytics& a);

// Writes all of the above into `dir` (created if needed; benchmark.csv only when a benchmark is given).
// Throws std::runtime_error if a file cannot be written. Files use LF line endings.
void export_run(const std::string& dir, const EngineResult& r, const Analytics& a, const BenchmarkSeries* benchmark);

}  // namespace bt
