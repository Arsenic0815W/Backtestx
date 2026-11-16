// bt_bench: throughput of the engine and of the sweep thread pool, on SYNTHETIC data.
//
//   bt_bench [--json] [--quick] [--threads 1,2,4,8] [--min-seconds 1.5] [--allow-unoptimized]
//
// What is timed (and what is not):
//   * Timed:   bt::run_backtest (engine + analytics) for single runs, bt::run_sweep for sweeps. Nothing else.
//   * Not timed: generating the synthetic data, aligning the calendar, building the grid, hashing results, printing.
//   * No file I/O and no report writing happen inside any timed region (the CLI is not involved).
//   * A "bar" is one symbol-day: symbols x calendar days (e.g. 50 symbols x 2520 days = 126,000 bars). Every bar of the data is
//     evaluated (there is no warm-up window in these workloads), so bars/sec counts all of them.
//   * Each timed unit is repeated; the FIRST repetition is discarded (cold caches, allocator growth); repetitions continue until
//     the workload has used --min-seconds of timed work and at least 5 repetitions were made. The median repetition is reported.
//   * Every repetition's result is hashed (equity curve, fills, trades, events) and the hashes must all agree; for sweeps the row
//     hashes of every grid point must agree across repetitions AND across thread counts. A benchmark whose answer changed would
//     be meaningless, so any mismatch aborts with a non-zero exit code. Trades must be > 0.
//   * The build must be optimised (__OPTIMIZE__); otherwise the program refuses unless --allow-unoptimized.
//   * --quick: tiny sizes, two repetitions each, checks only that everything runs and agrees. Never asserts on speed.
//
// Not investment advice. Synthetic data only; these numbers say nothing about real markets.
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#include "bt/runner.hpp"
#include "bt/sweep.hpp"
#include "bt/synthetic.hpp"
#include "bt/version.hpp"
#include "json.hpp"

using namespace bt;
using nlohmann::json;
using Clock = std::chrono::steady_clock;

namespace {

struct Options {
  bool json = false, quick = false, allow_unoptimized = false;
  std::vector<std::size_t> threads;  // empty = 1, 2, 4, N
  double min_seconds = 1.5;
};

[[noreturn]] void die(const std::string& msg) {
  std::fprintf(stderr, "bt_bench: FAILED: %s\n", msg.c_str());
  std::exit(1);
}

double median(std::vector<double> v) {
  std::sort(v.begin(), v.end());
  const std::size_t n = v.size();
  return n % 2 ? v[n / 2] : 0.5 * (v[n / 2 - 1] + v[n / 2]);
}

std::string hex(std::uint64_t h) {
  char b[24];
  std::snprintf(b, sizeof b, "%016llx", static_cast<unsigned long long>(h));
  return b;
}

std::vector<SymbolSeries> make_universe(int symbols, int days, std::uint64_t seed0) {
  std::vector<SymbolSeries> out;
  for (int i = 0; i < symbols; ++i) {
    SynthParams p;
    p.seed = seed0 + static_cast<std::uint64_t>(i);
    p.num_bars = days;
    p.start = days_from_civil(2005, 1, 3);
    char name[16];
    std::snprintf(name, sizeof name, "S%02d", i);
    out.push_back({name, generate_gbm(p)});
  }
  return out;
}

RunConfig base_config(const std::string& strategy) {
  RunConfig c;
  c.name = "bench";
  c.strategy = strategy;
  c.capital_rupees = 1'000'000.0;
  c.allocation = 0.95;
  c.benchmark.mode = BenchmarkSpec::Mode::None;
  return c;
}

struct Stats {
  double median_ms = 0, min_ms = 0, max_ms = 0;
  std::size_t reps = 0;
};

Stats summarise(const std::vector<double>& ms) {
  Stats s;
  s.reps = ms.size();
  s.median_ms = median(ms);
  s.min_ms = *std::min_element(ms.begin(), ms.end());
  s.max_ms = *std::max_element(ms.begin(), ms.end());
  return s;
}

// ---------------------------------------------------------------------------------------------- single backtest
json bench_single(const std::string& label, const std::string& strategy, const json& params, int symbols, int days, const Options& o) {
  const std::vector<SymbolSeries> series = make_universe(symbols, days, 1000);
  const AlignedData aligned = make_aligned(series, std::nullopt);
  const RunConfig cfg = base_config(strategy);
  const std::uint64_t bars = static_cast<std::uint64_t>(symbols) * aligned.dates().size();

  std::vector<double> ms;
  std::uint64_t hash = 0;
  std::size_t fills = 0, trades = 0;
  double spent = 0;
  const std::size_t min_reps = o.quick ? 2 : 5;
  for (std::size_t rep = 0;; ++rep) {
    const auto t0 = Clock::now();
    const Backtest bt = run_backtest(aligned, nullptr, cfg, params, std::nullopt);
    const auto t1 = Clock::now();
    const double dt = std::chrono::duration<double, std::milli>(t1 - t0).count();
    const std::uint64_t h = result_hash(bt.result);
    if (rep == 0) {
      hash = h;
      fills = bt.result.fills.size();
      trades = bt.result.trades.size();
      if (trades == 0) die(label + ": the workload produced no closed trades (it would measure an idle strategy)");
      if (!o.quick) continue;  // discard the first (cold) repetition, but remember its hash
    } else if (h != hash) {
      die(label + ": the result changed between repetitions (hash " + hex(hash) + " vs " + hex(h) + ")");
    }
    ms.push_back(dt);
    spent += dt / 1000.0;
    if (ms.size() >= min_reps && (o.quick || spent >= o.min_seconds)) break;
  }
  const Stats s = summarise(ms);
  return {{"name", label}, {"strategy", strategy}, {"params", params}, {"symbols", symbols}, {"calendar_days", aligned.dates().size()},
          {"bars", bars}, {"fills", fills}, {"trades", trades}, {"result_hash", hex(hash)}, {"reps", s.reps},
          {"median_ms", s.median_ms}, {"min_ms", s.min_ms}, {"max_ms", s.max_ms},
          {"bars_per_sec_median", static_cast<double>(bars) / (s.median_ms / 1000.0)},
          {"bars_per_sec_best", static_cast<double>(bars) / (s.min_ms / 1000.0)}};
}

// ---------------------------------------------------------------------------------------------------- sweep
std::uint64_t sweep_signature(const SweepRun& r, std::size_t* n_ok) {
  std::uint64_t h = 1469598103934665603ULL;  // FNV-1a over the row hashes in grid order
  *n_ok = 0;
  for (const SweepRow& row : r.rows) {
    if (row.status == "ok") ++*n_ok;
    const std::uint64_t x = row.status == "ok" ? row.hash : 0;
    for (int b = 0; b < 8; ++b) {
      h ^= (x >> (8 * b)) & 0xff;
      h *= 1099511628211ULL;
    }
  }
  return h;
}

json bench_sweep(const std::vector<std::size_t>& thread_counts, int days, const Options& o) {
  const std::vector<SymbolSeries> series = make_universe(1, days, 7000);
  const AlignedData aligned = make_aligned(series, std::nullopt);
  RunConfig cfg = base_config("sma_crossover");
  cfg.params = {{"symbol", "S00"}};

  SweepSpec spec;
  ParamAxis fast, slow;
  fast.name = "fast";
  slow.name = "slow";
  const int nf = o.quick ? 3 : 8, ns = o.quick ? 4 : 16;
  for (int i = 0; i < nf; ++i) fast.values.push_back(5 + 5 * i);
  for (int i = 0; i < ns; ++i) slow.values.push_back(50 + 10 * i);
  spec.axes = {fast, slow};
  const std::vector<GridPoint> grid = expand_grid(cfg.params, spec.axes, 100000);

  SweepContext ctx;  // NO in-sample / out-of-sample split: one grid point is exactly one backtest
  ctx.cfg = &cfg;
  ctx.is_data = &aligned;
  ctx.is_bench = nullptr;
  ctx.is_eval_start = std::nullopt;

  json per_thread = json::array();
  std::uint64_t signature = 0;
  bool have_signature = false;
  std::size_t ok_points = 0;
  for (const std::size_t th : thread_counts) {
    std::vector<double> ms;
    double spent = 0;
    const std::size_t min_reps = o.quick ? 2 : 5;
    std::size_t threads_used = 0;
    for (std::size_t rep = 0;; ++rep) {
      const auto t0 = Clock::now();
      const SweepRun run = run_sweep(ctx, grid, th);
      const auto t1 = Clock::now();
      const double dt = std::chrono::duration<double, std::milli>(t1 - t0).count();
      std::size_t n_ok = 0;
      const std::uint64_t sig = sweep_signature(run, &n_ok);
      if (!have_signature) {
        signature = sig;
        have_signature = true;
        ok_points = n_ok;
        std::size_t with_trades = 0;
        for (const SweepRow& r : run.rows)
          if (r.status == "ok" && r.is && r.is->num_trades > 0) ++with_trades;
        if (with_trades == 0) die("sweep: no grid point produced a closed trade");
      } else if (sig != signature || n_ok != ok_points) {
        die("sweep: results differ from the first run (threads " + std::to_string(th) + "): throughput of a run that changed its answer is meaningless");
      }
      threads_used = run.threads_used;
      if (rep == 0 && !o.quick) continue;  // discard the first repetition
      ms.push_back(dt);
      spent += dt / 1000.0;
      if (ms.size() >= min_reps && (o.quick || spent >= o.min_seconds)) break;
    }
    const Stats s = summarise(ms);
    per_thread.push_back({{"threads_requested", th}, {"threads_used", threads_used}, {"reps", s.reps}, {"median_ms", s.median_ms},
                          {"min_ms", s.min_ms}, {"max_ms", s.max_ms},
                          {"runs_per_sec_median", static_cast<double>(ok_points) / (s.median_ms / 1000.0)},
                          {"runs_per_sec_best", static_cast<double>(ok_points) / (s.min_ms / 1000.0)}});
  }
  return {{"name", "sweep sma_crossover, 1 symbol x " + std::to_string(days) + " days, no IS/OOS split"},
          {"grid_points", grid.size()}, {"runnable_points", ok_points}, {"bars_per_run", days}, {"signature", hex(signature)},
          {"per_thread", per_thread}};
}

std::vector<std::size_t> parse_list(const std::string& s) {
  std::vector<std::size_t> v;
  std::size_t i = 0;
  while (i < s.size()) {
    std::size_t j = s.find(',', i);
    if (j == std::string::npos) j = s.size();
    const long x = std::strtol(s.substr(i, j - i).c_str(), nullptr, 10);
    if (x < 1 || x > 1024) die("--threads takes a comma-separated list of integers 1..1024");
    v.push_back(static_cast<std::size_t>(x));
    i = j + 1;
  }
  return v;
}

}  // namespace

int main(int argc, char** argv) {
  Options o;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--json") o.json = true;
    else if (a == "--quick") o.quick = true;
    else if (a == "--allow-unoptimized") o.allow_unoptimized = true;
    else if (a == "--threads" && i + 1 < argc) o.threads = parse_list(argv[++i]);
    else if (a == "--min-seconds" && i + 1 < argc) o.min_seconds = std::atof(argv[++i]);
    else die("unknown or incomplete option '" + a + "'");
  }
#ifndef __OPTIMIZE__
  if (!o.allow_unoptimized) die("this build is not optimised (__OPTIMIZE__ is not defined); build Release or pass --allow-unoptimized");
#endif
  const std::size_t hw = std::max<std::size_t>(1, std::thread::hardware_concurrency());
  if (o.threads.empty()) {
    o.threads = {1, 2, 4, hw};
    o.threads.erase(std::unique(o.threads.begin(), o.threads.end()), o.threads.end());
    if (hw < 4) {  // keep the list strictly increasing and no larger than the machine
      o.threads.clear();
      for (std::size_t t = 1; t <= hw; t *= 2) o.threads.push_back(t);
      if (o.threads.back() != hw) o.threads.push_back(hw);
    }
  }

  json out;
  out["tool"] = std::string("bt_bench ") + version();
  out["optimised_build"] =
#ifdef __OPTIMIZE__
      true;
#else
      false;
#endif
  out["ndebug"] =
#ifdef NDEBUG
      true;
#else
      false;
#endif
  out["hardware_concurrency"] = hw;
  out["quick"] = o.quick;
  out["min_seconds"] = o.min_seconds;
  out["single"] = json::array();

  if (o.quick) {
    out["single"].push_back(bench_single("1 symbol x 2 years", "sma_crossover", {{"symbol", "S00"}, {"fast", 20}, {"slow", 50}}, 1, 504, o));
    json uni = json::array();
    for (int i = 0; i < 5; ++i) uni.push_back(std::string("S0") + std::to_string(i));
    out["single"].push_back(bench_single("5 symbols x 1 year", "momentum_rotation", {{"universe", uni}, {"lookback", 40}, {"top_k", 2}}, 5, 252, o));
    out["sweep"] = bench_sweep(o.threads, 504, o);
  } else {
    out["single"].push_back(bench_single("1 symbol x 20 years", "sma_crossover", {{"symbol", "S00"}, {"fast", 20}, {"slow", 50}}, 1, 5040, o));
    json uni = json::array();
    for (int i = 0; i < 50; ++i) {
      char n[16];
      std::snprintf(n, sizeof n, "S%02d", i);
      uni.push_back(n);
    }
    out["single"].push_back(bench_single("50 symbols x 10 years", "momentum_rotation", {{"universe", uni}, {"lookback", 126}, {"top_k", 5}}, 50, 2520, o));
    out["sweep"] = bench_sweep(o.threads, 2520, o);
  }

  if (o.json) {
    std::printf("%s\n", out.dump(2).c_str());
  } else {
    std::printf("bt_bench %s (%s build, %zu hardware threads)%s\n", version(), out["optimised_build"].get<bool>() ? "optimised" : "UNOPTIMISED", hw,
                o.quick ? " --quick" : "");
    for (const json& s : out["single"])
      std::printf("  %-24s %9llu bars  %6zu fills %5zu trades  median %9.2f ms  %12.0f bars/s (best %12.0f)  hash %s\n", s["name"].get<std::string>().c_str(),
                  static_cast<unsigned long long>(s["bars"].get<std::uint64_t>()), s["fills"].get<std::size_t>(), s["trades"].get<std::size_t>(),
                  s["median_ms"].get<double>(), s["bars_per_sec_median"].get<double>(), s["bars_per_sec_best"].get<double>(),
                  s["result_hash"].get<std::string>().c_str());
    const json& sw = out["sweep"];
    std::printf("  %s: %zu points (%zu runnable)\n", sw["name"].get<std::string>().c_str(), sw["grid_points"].get<std::size_t>(), sw["runnable_points"].get<std::size_t>());
    for (const json& t : sw["per_thread"])
      std::printf("    threads %3zu (used %3zu): median %9.2f ms  %9.1f runs/s (best %9.1f)\n", t["threads_requested"].get<std::size_t>(),
                  t["threads_used"].get<std::size_t>(), t["median_ms"].get<double>(), t["runs_per_sec_median"].get<double>(), t["runs_per_sec_best"].get<double>());
  }
  if (o.quick) std::fprintf(stderr, "bt_bench --quick OK: every workload ran, results identical across repetitions and thread counts (speed not asserted)\n");
  return 0;
}
