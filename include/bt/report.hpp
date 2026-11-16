#pragma once
// The self-contained HTML report (Phase 6).
//
//   run folder   (summary.json run.json equity.csv trades.csv fills.csv [benchmark.csv])  ->  report.html
//   sweep folder (sweep_summary.json sweep.csv [best/])                                    ->  report.html
//
// ONE code path: a report is built ONLY from the files in the folder, never from in-memory results, so it cannot disagree with
// the exported files and tools/check_report.py can verify it against them. `run`, `sweep` and `backtestx report --dir` all call
// the same functions.
//
// Every number in the report is computed HERE (C++) and embedded as JSON; the JavaScript in src/report_assets/report.js only
// formats and draws. Report-only data (drawdown series, monthly/yearly returns, fee aggregates, the benchmark's own metrics) lives
// only in the HTML: summary.json stays EXACTLY the analytics (tools/ref_metrics.py rejects extra sections).
//
// Definitions (also pinned by tests/test_report.cpp and re-derived independently by tools/check_report.py):
//   monthly return  last equity record of the month / last record of the previous month - 1; the first month starts from the first
//                   record of the curve. The first and last month are flagged (they may cover only part of a calendar month).
//   yearly return   the same rule per calendar year (equal to the compounded monthly returns).
//   drawdown        E_i / (running maximum of E up to i) - 1; null if the running maximum is not positive.
//   benchmark curve the benchmark values on the dates the strategy curve and the benchmark file share, rescaled so that the first
//                   common date equals the strategy equity there: B_c = round(E_c0 * v_c / v_c0) paise (llround). Its own metrics
//                   come from the SAME analyze() code path (returns between consecutive common dates).
//   fees            totals of the fills.csv fee columns; by symbol; by side. The components must add up to fees_paise or the
//                   builder throws.
// Paths are shortened to file names inside the report (the full paths stay in run.json).
// Not investment advice. Past performance does not predict future results.
#include <optional>
#include <string>
#include <vector>

#include "bt/analytics.hpp"
#include "bt/engine.hpp"
#include "json.hpp"

namespace bt {

struct MonthlyReturn {
  int year = 0, month = 0;
  std::optional<double> ret;
  Date from = 0, to = 0;  // the base record's date and the month's last record's date
  bool first = false, last = false;
};
struct YearlyReturn {
  int year = 0;
  std::optional<double> ret;
  Date from = 0, to = 0;
};
std::vector<MonthlyReturn> monthly_returns(const std::vector<DailyRecord>& curve);
std::vector<YearlyReturn> yearly_returns(const std::vector<DailyRecord>& curve);
std::vector<std::optional<double>> drawdown_series(const std::vector<Paise>& equity);

struct RebasedBenchmark {
  std::vector<Date> dates;
  std::vector<Paise> equity;
};
// nullopt when the curve and the benchmark share no date.
std::optional<RebasedBenchmark> rebase_benchmark(const std::vector<DailyRecord>& curve, const BenchmarkSeries& bench);

// A JSON document as it is embedded in a <script> element: ASCII only, and every '<' written as < so that "</script",
// "<!--" and similar can never end or confuse the element.
std::string script_safe_json(const nlohmann::json& j);
std::string html_escape(const std::string& s);

bool is_sweep_dir(const std::string& dir);  // sweep_summary.json present
// Throw std::runtime_error naming the missing / inconsistent file.
nlohmann::json build_run_report_data(const std::string& dir);
nlohmann::json build_sweep_report_data(const std::string& dir);
std::string render_report_html(const nlohmann::json& data);

void write_run_report(const std::string& dir);     // dir/report.html
void write_sweep_reports(const std::string& dir);  // dir/best/report.html (when best/ exists), then dir/report.html

}  // namespace bt
