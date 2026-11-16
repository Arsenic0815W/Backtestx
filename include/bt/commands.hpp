#pragma once
// The command-line commands, kept in the library so tests can call them in-process (main() is a thin dispatcher).
//
//   backtestx run   --config FILE --out DIR              one backtest; writes equity/trades/fills/benchmark CSVs, summary.json, run.json
//   backtestx sweep --config FILE --out DIR [--threads N] a parameter grid (+ optional out-of-sample split); writes sweep.csv,
//                                                          sweep_summary.json and best/ (a full run folder)
//   backtestx check FILE [--skip-null] [--adjust]         validate a CSV
//   backtestx synth --out FILE [--seed N] [--bars N] [--start YYYY-MM-DD] [--price P]   seeded synthetic data (NOT real prices)
//   backtestx --version
//
// Exit codes: 0 success; 1 runtime / data error (or a sweep where no combination qualifies); 2 usage or configuration error.
// Not investment advice. Past performance does not predict future results.
#include <iosfwd>
#include <string>
#include <vector>

namespace bt {

int cli_main(const std::vector<std::string>& args, std::ostream& out, std::ostream& err);
int cmd_run(const std::vector<std::string>& args, std::ostream& out, std::ostream& err);
int cmd_sweep(const std::vector<std::string>& args, std::ostream& out, std::ostream& err);
int cmd_report(const std::vector<std::string>& args, std::ostream& out, std::ostream& err);
int cmd_check(const std::vector<std::string>& args, std::ostream& out, std::ostream& err);
int cmd_synth(const std::vector<std::string>& args, std::ostream& out, std::ostream& err);

}  // namespace bt
