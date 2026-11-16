#include "bt/commands.hpp"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>

#include "bt/adjust.hpp"
#include "bt/csv.hpp"
#include "bt/report.hpp"
#include "bt/runner.hpp"
#include "bt/sweep.hpp"
#include "bt/synthetic.hpp"
#include "bt/version.hpp"

namespace bt {
namespace {

std::string pct(const std::optional<double>& v) {
  if (!v) return "n/a";
  char b[32];
  std::snprintf(b, sizeof b, "%.2f%%", *v * 100.0);
  return b;
}
std::string num(const std::optional<double>& v) {
  if (!v) return "n/a";
  char b[32];
  std::snprintf(b, sizeof b, "%.3f", *v);
  return b;
}

void usage(std::ostream& o) {
  o << "backtestx " << version() << "  (research / education software; not investment advice)\n"
       "  backtestx run   --config FILE --out DIR\n"
       "  backtestx sweep --config FILE --out DIR [--threads N]\n"
       "  backtestx report --dir DIR\n"
       "  backtestx check FILE [--skip-null] [--adjust]\n"
       "  backtestx synth --out FILE [--seed N] [--bars N] [--start YYYY-MM-DD] [--price P]\n"
       "  backtestx --version\n";
}

// Parses "--key value" pairs; returns false (and prints) on an unknown or incomplete option.
bool parse_opts(const std::vector<std::string>& a, std::initializer_list<const char*> known,
                std::vector<std::pair<std::string, std::string>>& out, std::ostream& err) {
  for (std::size_t i = 0; i < a.size(); i += 2) {
    bool ok = false;
    for (const char* k : known)
      if (a[i] == k) ok = true;
    if (!ok || i + 1 >= a.size()) {
      err << "error: bad or incomplete option '" << a[i] << "'\n";
      return false;
    }
    out.emplace_back(a[i], a[i + 1]);
  }
  return true;
}
std::string opt(const std::vector<std::pair<std::string, std::string>>& o, const std::string& k) {
  for (const auto& kv : o)
    if (kv.first == k) return kv.second;
  return "";
}

void print_run_summary(std::ostream& out, const RunConfig& cfg, const Backtest& bt, const std::string& dir) {
  const EquityMetrics& e = bt.analytics.equity;
  const TradeMetrics& t = bt.analytics.trades;
  out << "run '" << cfg.name << "': " << cfg.strategy << ", " << format_date(*e.start_date) << " .. " << format_date(*e.end_date)
      << " (" << e.n_records << " days, " << bt.result.warmup_days << " warm-up days before the start)\n"
      << "  capital Rs " << format_paise(*e.start_equity) << " -> Rs " << format_paise(*e.end_equity) << "   total return " << pct(e.total_return)
      << "   CAGR " << pct(e.cagr) << "   volatility " << pct(e.annualised_volatility) << "\n"
      << "  Sharpe " << num(e.sharpe) << "   Sortino " << num(e.sortino) << "   max drawdown " << pct(e.max_drawdown)
      << "   Calmar " << num(e.calmar) << "\n"
      << "  trades " << t.num_trades << " (win rate " << pct(t.win_rate) << ", profit factor " << num(t.profit_factor) << ")   fees Rs "
      << format_paise(t.total_fees_paise) << "   open positions " << t.num_open_positions << "\n";
  if (bt.analytics.benchmark) {
    const BenchmarkMetrics& b = *bt.analytics.benchmark;
    out << "  vs " << b.name << ": excess return " << pct(b.excess_return) << "   beta " << num(b.beta) << "   alpha(annual) " << pct(b.alpha_annual)
        << "   correlation " << num(b.correlation) << "   (" << b.n_pairs << " paired returns)\n";
  }
  out << "  outputs: " << dir << "\n  Not investment advice. Past performance does not predict future results.\n";
}

}  // namespace

// ------------------------------------------------------------------------------------------------------- run
int cmd_run(const std::vector<std::string>& args, std::ostream& out, std::ostream& err) {
  std::vector<std::pair<std::string, std::string>> o;
  if (!parse_opts(args, {"--config", "--out"}, o, err)) return 2;
  const std::string file = opt(o, "--config"), dir = opt(o, "--out");
  if (file.empty() || dir.empty()) {
    err << "error: run needs --config FILE and --out DIR\n";
    return 2;
  }
  try {
    const RunConfig cfg = load_run_config(file);
    const LoadedRun data = load_run_data(cfg);
    const AlignedData aligned = make_aligned(data.series, cfg.end);
    std::optional<BenchmarkSeries> bench;
    if (data.benchmark) bench = make_benchmark(*data.benchmark, cfg.end);
    const Backtest bt = run_backtest(aligned, bench ? &*bench : nullptr, cfg, cfg.params, cfg.start);

    std::vector<std::string> warnings;
    if (cfg.start && bt.result.warmup_days == 0)
      warnings.push_back("no data before the start date: strategies warm up INSIDE the evaluation window (they will sit in cash at first)");
    nlohmann::json used = cfg.params;
    if (cfg.allocation) used["allocation"] = *cfg.allocation;
    write_run_outputs(dir, cfg, data, bt, bench ? &*bench : nullptr, used, warnings);
    for (const std::string& w : warnings) err << "WARNING: " << w << "\n";
    // The report is built from the files just written. If it fails, the data files stay and the exit code is non-zero.
    std::string report_error;
    try {
      write_run_report(dir);
    } catch (const std::exception& e) {
      report_error = e.what();
    }
    print_run_summary(out, cfg, bt, dir);
    if (!report_error.empty()) {
      err << "error: the data files were written but the HTML report was not: " << report_error << "\n";
      return 1;
    }
    out << "  report: " << dir << "/report.html\n";
    return 0;
  } catch (const ConfigError& e) {
    err << "config error: " << e.what() << "\n";
    return 2;
  } catch (const std::exception& e) {
    err << "error: " << e.what() << "\n";
    return 1;
  }
}

// ----------------------------------------------------------------------------------------------------- sweep
int cmd_sweep(const std::vector<std::string>& args, std::ostream& out, std::ostream& err) {
  std::vector<std::pair<std::string, std::string>> o;
  if (!parse_opts(args, {"--config", "--out", "--threads"}, o, err)) return 2;
  const std::string file = opt(o, "--config"), dir = opt(o, "--out"), th = opt(o, "--threads");
  if (file.empty() || dir.empty()) {
    err << "error: sweep needs --config FILE and --out DIR\n";
    return 2;
  }
  std::size_t threads = 0;
  if (!th.empty()) {
    char* end = nullptr;
    const long v = std::strtol(th.c_str(), &end, 10);
    if (*end != '\0' || v < 0 || v > 1024) {
      err << "error: --threads must be an integer 0..1024 (0 = all cores)\n";
      return 2;
    }
    threads = static_cast<std::size_t>(v);
  }
  try {
    nlohmann::json block;
    const RunConfig cfg = [&] {
      nlohmann::json sb;
      RunConfig c = load_run_config(file, &sb);
      block = sb;
      return c;
    }();
    if (block.is_null()) throw ConfigError(file + ": a sweep needs a \"sweep\" block (use `backtestx run` for a single backtest)");
    SweepSpec spec;
    try {
      spec = parse_sweep_spec(block);
    } catch (const ConfigError& e) {
      throw ConfigError(file + ": " + e.what());
    }
    const LoadedRun data = load_run_data(cfg);
    const SweepOutcome r = execute_sweep(cfg, spec, data, threads, dir);

    std::size_t ok = 0, bad = 0;
    for (const SweepRow& row : r.rows) (row.status == "ok" ? ok : bad)++;
    out << "sweep '" << cfg.name << "': " << r.rows.size() << " combinations (" << ok << " ran, " << bad << " invalid/failed) on "
        << r.threads_used << " thread(s); selection metric " << metric_name(spec.metric) << ", min_trades " << spec.min_trades << "\n";
    if (r.best) {
      const SweepRow& b = r.rows[*r.best];
      out << "  best (selected on the " << (spec.walk_forward ? "IN-SAMPLE" : "whole") << " window): run " << b.id << " " << b.axis.dump() << "\n"
          << "    in-sample:  Sharpe " << num(b.is->sharpe) << "  CAGR " << pct(b.is->cagr) << "  max drawdown " << pct(b.is->max_drawdown)
          << "  trades " << b.is->num_trades << "\n";
      if (b.oos)
        out << "    out-of-sample: Sharpe " << num(b.oos->sharpe) << "  CAGR " << pct(b.oos->cagr) << "  max drawdown " << pct(b.oos->max_drawdown)
            << "  trades " << b.oos->num_trades << "\n";
    }
    for (const std::string& w : r.warnings) err << w << "\n";
    std::string report_error;
    try {
      write_sweep_reports(dir);
    } catch (const std::exception& e) {
      report_error = e.what();
    }
    out << "  outputs: " << dir << " (sweep.csv, sweep_summary.json, best/, report.html)\n  Not investment advice. Past performance does not predict future results.\n";
    if (!report_error.empty()) {
      err << "error: the data files were written but the HTML report was not: " << report_error << "\n";
      return 1;
    }
    return r.best ? 0 : 1;
  } catch (const ConfigError& e) {
    err << "config error: " << e.what() << "\n";
    return 2;
  } catch (const std::exception& e) {
    err << "error: " << e.what() << "\n";
    return 1;
  }
}

// ----------------------------------------------------------------------------------------------------- check
int cmd_check(const std::vector<std::string>& a, std::ostream& out, std::ostream& err) {
  if (a.empty()) {
    usage(err);
    return 2;
  }
  LoadOptions opts;
  bool adjust = false;
  for (std::size_t i = 1; i < a.size(); ++i) {
    if (a[i] == "--skip-null") opts.null_policy = NullPolicy::Skip;
    else if (a[i] == "--adjust") adjust = true;
    else {
      err << "unknown option " << a[i] << "\n";
      return 2;
    }
  }
  LoadResult r = load_csv_file(a[0], opts);
  if (!r.ok()) {
    err << format_error(a[0], *r.error) << "\n";
    return 1;
  }
  for (const auto& w : r.warnings) err << "warning: " << w.message << "\n";
  std::vector<Bar> bars = adjust ? back_adjust(r.bars) : r.bars;
  if (adjust) {  // adjusted output must satisfy the same bar invariants as the raw input
    for (const Bar& b : bars) {
      if (const auto e = check_bar(b)) {
        err << a[0] << ": adjusted bar " << format_date(b.date) << " fails check: " << to_string(*e) << "\n";
        return 1;
      }
    }
  }
  out << a[0] << ": " << bars.size() << " bars, " << format_date(bars.front().date) << " .. " << format_date(bars.back().date) << ", "
      << r.warnings.size() << " skipped null rows" << (adjust ? " (back-adjusted)" : "") << "\n";
  return 0;
}

// ----------------------------------------------------------------------------------------------------- synth
int cmd_synth(const std::vector<std::string>& a, std::ostream& out, std::ostream& err) {
  SynthParams p;
  std::string path;
  for (std::size_t i = 0; i < a.size(); ++i) {
    const bool has_val = i + 1 < a.size();
    if (a[i] == "--out" && has_val) path = a[++i];
    else if (a[i] == "--seed" && has_val) p.seed = std::strtoull(a[++i].c_str(), nullptr, 10);
    else if (a[i] == "--bars" && has_val) p.num_bars = std::atoi(a[++i].c_str());
    else if (a[i] == "--price" && has_val) p.start_price = std::atof(a[++i].c_str());
    else if (a[i] == "--start" && has_val) {
      const auto d = parse_date(a[++i]);
      if (!d) {
        err << "bad --start date\n";
        return 2;
      }
      p.start = *d;
    } else {
      err << "bad or incomplete option " << a[i] << "\n";
      return 2;
    }
  }
  if (path.empty() || p.num_bars <= 0 || p.start_price <= 0) {
    usage(err);
    return 2;
  }
  const std::vector<Bar> bars = generate_gbm(p);
  std::ofstream f(path, std::ios::binary);  // binary: keep LF line endings on Windows
  if (!f) {
    err << "cannot write " << path << "\n";
    return 1;
  }
  write_csv(f, bars);
  f.flush();
  if (!f) {
    err << "write failed for " << path << "\n";
    return 1;
  }
  out << "wrote " << bars.size() << " bars to " << path << "\n";
  return 0;
}

// -------------------------------------------------------------------------------------------------- dispatch
int cmd_report(const std::vector<std::string>& args, std::ostream& out, std::ostream& err) {
  std::vector<std::pair<std::string, std::string>> o;
  if (!parse_opts(args, {"--dir"}, o, err)) return 2;
  const std::string dir = opt(o, "--dir");
  if (dir.empty()) {
    err << "error: report needs --dir DIR (a run or sweep output folder)\n";
    return 2;
  }
  try {
    if (is_sweep_dir(dir)) write_sweep_reports(dir);
    else write_run_report(dir);
    out << "wrote " << dir << "/report.html\n";
    return 0;
  } catch (const std::exception& e) {
    err << "error: " << e.what() << "\n";
    return 1;
  }
}

int cli_main(const std::vector<std::string>& args, std::ostream& out, std::ostream& err) {
  if (args.empty()) {
    usage(err);
    return 2;
  }
  const std::string cmd = args.front();
  const std::vector<std::string> rest(args.begin() + 1, args.end());
  if (cmd == "--version") {
    out << "backtestx " << version() << "\n";
    return 0;
  }
  if (cmd == "--help" || cmd == "-h") {
    usage(out);
    return 0;
  }
  if (cmd == "run") return cmd_run(rest, out, err);
  if (cmd == "sweep") return cmd_sweep(rest, out, err);
  if (cmd == "report") return cmd_report(rest, out, err);
  if (cmd == "check") return cmd_check(rest, out, err);
  if (cmd == "synth") return cmd_synth(rest, out, err);
  err << "unknown command '" << cmd << "'\n";
  usage(err);
  return 2;
}

}  // namespace bt
