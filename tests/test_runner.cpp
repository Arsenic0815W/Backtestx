// End-to-end tests of `backtestx run` / `sweep` through real (temp) files, called in-process via bt::cmd_*.
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "bt/adjust.hpp"
#include "bt/commands.hpp"
#include "bt/csv.hpp"
#include "bt/money.hpp"
#include "bt/synthetic.hpp"
#include "bt/version.hpp"
#include "doctest.h"
#include "json.hpp"

using namespace bt;
using nlohmann::json;
namespace fs = std::filesystem;

namespace {

struct Project {
  fs::path root;
  explicit Project(const char* name) : root(fs::temp_directory_path() / (std::string("bt_test_proj_") + name)) {
    fs::remove_all(root);
    fs::create_directories(root / "configs");
    fs::create_directories(root / "data");
  }
  ~Project() { fs::remove_all(root); }

  void csv(const char* file, const std::vector<Bar>& bars) const {
    std::ofstream f(root / "data" / file, std::ios::binary);
    write_csv(f, bars);
  }
  void raw(const char* file, const std::string& text) const { std::ofstream(root / "data" / file, std::ios::binary) << text; }
  std::string config(const char* name, const std::string& text) const {
    std::ofstream(root / "configs" / name, std::ios::binary) << text;
    return (root / "configs" / name).string();
  }
  std::string out(const char* name) const { return (root / "out" / name).string(); }
};

struct Result {
  int code;
  std::string out, err;
};

Result run_cmd(int (*fn)(const std::vector<std::string>&, std::ostream&, std::ostream&), const std::vector<std::string>& args) {
  std::ostringstream o, e;
  const int c = fn(args, o, e);
  return {c, o.str(), e.str()};
}
Result run(const std::string& cfg, const std::string& out) { return run_cmd(cmd_run, {"--config", cfg, "--out", out}); }

std::vector<Bar> gbm(std::uint64_t seed, int bars, double price, double vol = 0.25, const char* start = "2021-01-04") {
  SynthParams p;
  p.start = parse_date(start).value();
  p.num_bars = bars;
  p.seed = seed;
  p.start_price = price;
  p.annual_vol = vol;
  return generate_gbm(p);
}

std::string slurp(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

bool has(const std::string& s, const char* part) { return s.find(part) != std::string::npos; }

// price_paise of the first fill in fills.csv
long long first_fill_price(const std::string& dir) {
  std::istringstream in(slurp(fs::path(dir) / "fills.csv"));
  std::string line;
  std::getline(in, line);
  std::getline(in, line);
  std::vector<std::string> c;
  std::stringstream ls(line);
  std::string x;
  while (std::getline(ls, x, ',')) c.push_back(x);
  return std::stoll(c.at(6));
}

const char* kSma = R"("strategy": {"name": "sma_crossover", "params": {"fast": 5, "slow": 20}})";

}  // namespace

TEST_CASE("run: end to end - summary.json is exactly the analytics, run.json holds everything else") {
  const Project p("e2e");
  p.csv("AAA.csv", gbm(1, 400, 100));
  p.csv("NIFTYBEES.NS.csv", gbm(2, 400, 250, 0.15));          // found at the DEFAULT location ../data/NIFTYBEES.NS.csv
  const std::string cfg = p.config("r.json", std::string(R"({"data": {"files": ["AAA"]}, )") + kSma + "}");
  const Result r = run(cfg, p.out("a"));
  INFO(r.err);
  REQUIRE(r.code == 0);
  CHECK(has(r.out, "Not investment advice"));
  CHECK(has(r.out, "NIFTYBEES.NS"));

  const fs::path d = p.out("a");
  for (const char* f : {"equity.csv", "trades.csv", "fills.csv", "benchmark.csv", "summary.json", "run.json"}) CHECK(fs::exists(d / f));
  const json sum = json::parse(slurp(d / "summary.json"));
  std::vector<std::string> keys;
  for (auto it = sum.begin(); it != sum.end(); ++it) keys.push_back(it.key());
  CHECK(keys == std::vector<std::string>{"benchmark", "config", "equity", "trades"});   // nothing else: ref_metrics.py rejects extras
  CHECK(sum["benchmark"]["name"] == "NIFTYBEES.NS");

  const json rj = json::parse(slurp(d / "run.json"));
  CHECK(rj["tool"] == "backtestx");
  CHECK(rj["version"] == version());
  CHECK(rj["config"]["strategy"]["name"] == "sma_crossover");
  CHECK(rj["config"]["data"]["prices"] == "adjusted");
  CHECK(rj["config"]["benchmark"]["mode"] == "default");
  CHECK(rj["data"]["traded"][0]["symbol"] == "AAA");
  CHECK(rj["data"]["traded"][0]["bars"] == 400);
  CHECK(rj["data"]["benchmark"]["symbol"] == "NIFTYBEES.NS");
  CHECK(rj["window"]["warmup_days"] == 0);
  CHECK(rj["window"]["evaluation_days"] == 400);
  CHECK(rj["window"]["first_evaluation_date"] == "2021-01-04");
}

TEST_CASE("run: the DEFAULT benchmark file missing is a loud error; null is the explicit opt-out") {
  const Project p("nobench");
  p.csv("AAA.csv", gbm(1, 300, 100));
  const std::string base = R"({"data": {"files": ["AAA"]}, )" + std::string(kSma);
  const Result miss = run(p.config("a.json", base + "}"), p.out("a"));
  CHECK(miss.code == 1);
  CHECK(has(miss.err, "NIFTYBEES.NS"));
  CHECK(has(miss.err, "\"benchmark\": null"));                // the message tells the user how to opt out
  CHECK_FALSE(fs::exists(fs::path(p.out("a")) / "summary.json"));
  // an explicit benchmark that is missing is also an error (no quiet fallback)
  const Result miss2 = run(p.config("b.json", base + R"(, "benchmark": "OTHER"})"), p.out("b"));
  CHECK(miss2.code == 1);
  CHECK(has(miss2.err, "OTHER"));
  // explicit opt-out: runs, no benchmark file, summary has a null benchmark
  const Result none = run(p.config("c.json", base + R"(, "benchmark": null})"), p.out("c"));
  INFO(none.err);
  REQUIRE(none.code == 0);
  CHECK_FALSE(fs::exists(fs::path(p.out("c")) / "benchmark.csv"));
  CHECK(json::parse(slurp(fs::path(p.out("c")) / "summary.json"))["benchmark"].is_null());
  CHECK(json::parse(slurp(fs::path(p.out("c")) / "run.json"))["config"]["benchmark"].is_null());
}

TEST_CASE("run: prices are ADJUSTED by default through the config - traded symbol and benchmark - and 'raw' gives the file's own prices") {
  const Project p("adjusted");
  // Adj Close = 0.5 x Close on every row of both files: the adjusted series is half the raw one.
  std::vector<Bar> a = gbm(1, 300, 200), b = gbm(2, 300, 400, 0.15);
  for (Bar& x : a) x.adj_close = x.close * 0.5;
  for (Bar& x : b) x.adj_close = x.close * 0.5;
  p.csv("AAA.csv", a);
  p.csv("IDX.csv", b);
  const std::string base = R"({"data": {"files": ["AAA"]}, "benchmark": "IDX", "strategy": {"name": "buy_and_hold"})";

  // default: adjusted. buy_and_hold decides at bar 0's close and fills at bar 1's OPEN.
  const Result def = run(p.config("d.json", base + "}"), p.out("d"));
  INFO(def.err);
  REQUIRE(def.code == 0);
  CHECK(first_fill_price(p.out("d")) == to_paise(back_adjust(a)[1].open));
  CHECK(first_fill_price(p.out("d")) != to_paise(a[1].open));
  // the benchmark file written next to the results is the ADJUSTED series too
  const std::string bcsv = slurp(fs::path(p.out("d")) / "benchmark.csv");
  const std::string row1 = bcsv.substr(bcsv.find('\n') + 1, bcsv.find('\n', bcsv.find('\n') + 1) - bcsv.find('\n') - 1);
  CHECK(std::stod(row1.substr(row1.find(',') + 1)) == doctest::Approx(back_adjust(b)[0].close).epsilon(1e-9));
  CHECK(std::stod(row1.substr(row1.find(',') + 1)) == doctest::Approx(b[0].close * 0.5).epsilon(1e-9));

  // explicit "adjusted" is the same
  const Result adj = run(p.config("e.json", R"({"data": {"files": ["AAA"], "prices": "adjusted"}, "benchmark": "IDX", "strategy": {"name": "buy_and_hold"}})"), p.out("e"));
  REQUIRE(adj.code == 0);
  CHECK(first_fill_price(p.out("e")) == first_fill_price(p.out("d")));

  // raw: the file's own Close
  const Result raw = run(p.config("f.json", R"({"data": {"files": ["AAA"], "prices": "raw"}, "benchmark": "IDX", "strategy": {"name": "buy_and_hold"}})"), p.out("f"));
  REQUIRE(raw.code == 0);
  CHECK(first_fill_price(p.out("f")) == to_paise(a[1].open));
  const std::string rcsv = slurp(fs::path(p.out("f")) / "benchmark.csv");
  const std::string rrow = rcsv.substr(rcsv.find('\n') + 1, rcsv.find('\n', rcsv.find('\n') + 1) - rcsv.find('\n') - 1);
  CHECK(std::stod(rrow.substr(rrow.find(',') + 1)) == doctest::Approx(b[0].close).epsilon(1e-9));
  CHECK(json::parse(slurp(fs::path(p.out("f")) / "run.json"))["config"]["data"]["prices"] == "raw");
}

TEST_CASE("run: a glitch in the benchmark file stops the run; excluding the dates fixes it; a mistyped exclusion is an error") {
  const Project p("glitch");
  p.csv("AAA.csv", gbm(1, 300, 100));
  std::vector<Bar> idx = gbm(2, 300, 250, 0.1);
  const Date bad1 = idx[100].date, bad2 = idx[101].date;
  idx[100].open = idx[100].high = idx[100].low = idx[100].close = idx[100].adj_close = 25.0;     // ~-90 % for two days
  idx[101].open = idx[101].high = idx[101].low = idx[101].close = idx[101].adj_close = 25.2;
  p.csv("NIFTYBEES.NS.csv", idx);
  const std::string base = R"({"data": {"files": ["AAA"])" + std::string("%EXCL%") + "}, " + kSma + "}";
  auto cfg = [&](const std::string& excl) {
    std::string t = base;
    t.replace(t.find("%EXCL%"), 6, excl);
    return t;
  };
  const Result bad = run(p.config("a.json", cfg("")), p.out("a"));
  CHECK(bad.code == 1);
  CHECK(has(bad.err, format_date(bad1).c_str()));
  CHECK(has(bad.err, "exclude_dates"));
  CHECK(has(bad.err, "max_daily_move"));

  const std::string ex = R"(, "exclude_dates": {"NIFTYBEES.NS": [")" + format_date(bad1) + R"(", ")" + format_date(bad2) + R"("]})";
  const Result good = run(p.config("b.json", cfg(ex)), p.out("b"));
  INFO(good.err);
  REQUIRE(good.code == 0);
  const json rj = json::parse(slurp(fs::path(p.out("b")) / "run.json"));
  CHECK(rj["data"]["benchmark"]["excluded_dates"] == json::array({format_date(bad1), format_date(bad2)}));   // recorded
  CHECK(rj["data"]["benchmark"]["bars"] == 298);
  // the two dates became gaps in the benchmark pairing
  const json sum = json::parse(slurp(fs::path(p.out("b")) / "summary.json"));
  CHECK(sum["benchmark"]["n_curve_dates_without_benchmark"] == 2);

  // an exclusion date that is not a bar of the file: error (typo protection)
  const Result typo = run(p.config("c.json", cfg(R"(, "exclude_dates": {"NIFTYBEES.NS": ["1999-01-01"]})")), p.out("c"));
  CHECK(typo.code == 1);
  CHECK(has(typo.err, "1999-01-01"));
  CHECK(has(typo.err, "not a bar"));
  // an exclusion for an unknown symbol: configuration error
  const Result unk = run(p.config("d.json", cfg(R"(, "exclude_dates": {"ZZZ": ["2021-03-01"]})")), p.out("d"));
  CHECK(unk.code == 2);
  CHECK(has(unk.err, "data.exclude_dates.ZZZ"));
}

TEST_CASE("run: the evaluation window - start/end honoured, history before start is warm-up, nothing trades before start") {
  const Project p("window");
  const std::vector<Bar> a = gbm(1, 500, 100);
  p.csv("AAA.csv", a);
  const std::string start = format_date(a[200].date), end = format_date(a[420].date);
  const std::string cfg = R"({"data": {"files": ["AAA"]}, "benchmark": null, "period": {"start": ")" + start + R"(", "end": ")" + end + R"("}, )" +
                          std::string(kSma) + "}";
  const Result r = run(p.config("w.json", cfg), p.out("w"));
  INFO(r.err);
  REQUIRE(r.code == 0);
  const std::string eq = slurp(fs::path(p.out("w")) / "equity.csv");
  const std::size_t nl = eq.find('\n') + 1;
  CHECK(eq.substr(nl, 10) == start);                            // the curve starts at the start date ...
  const std::size_t last_nl = eq.rfind('\n', eq.size() - 2) + 1;
  CHECK(eq.substr(last_nl, 10) == end);                         // ... and ends at the end date
  const json rj = json::parse(slurp(fs::path(p.out("w")) / "run.json"));
  CHECK(rj["window"]["warmup_days"] == 200);
  CHECK(rj["window"]["evaluation_days"] == 221);
  CHECK(rj["window"]["requested_start"] == start);
  CHECK(rj["window"]["warmup_orders_discarded"].get<int>() >= 1);   // the strategy wanted to trade during warm-up; those orders were thrown away
  // initial cash at the start; no fill dated on or before the start
  CHECK(json::parse(slurp(fs::path(p.out("w")) / "summary.json"))["equity"]["start_equity_paise"] == 100'000'000);
  std::istringstream fl(slurp(fs::path(p.out("w")) / "fills.csv"));
  std::string line;
  std::getline(fl, line);
  while (std::getline(fl, line)) CHECK(line.substr(line.find(',') + 1, 10) > start);
  // a start with no earlier data warns that strategies will warm up inside the window
  const Result nowarm = run(p.config("x.json", R"({"data": {"files": ["AAA"]}, "benchmark": null, "period": {"start": ")" + format_date(a[0].date) +
                                                   R"("}, )" + kSma + "}"), p.out("x"));
  CHECK(nowarm.code == 0);
  CHECK(has(nowarm.err, "warm up INSIDE"));
  // a start after all the data: a clear error, not a crash
  const Result empty = run(p.config("y.json", R"({"data": {"files": ["AAA"]}, "benchmark": null, "period": {"start": "2040-01-01"}, )" + std::string(kSma) + "}"), p.out("y"));
  CHECK(empty.code == 1);
  CHECK(has(empty.err, "no data"));
}

TEST_CASE("run: configuration errors exit 2 with the key path; data errors exit 1; usage errors exit 2") {
  const Project p("errors");
  p.csv("AAA.csv", gbm(1, 300, 100));
  const std::string good = R"({"data": {"files": ["AAA"]}, "benchmark": null, "strategy": {"name": "buy_and_hold"})";
  Result r = run(p.config("a.json", good + R"(, "capitol": 5})"), p.out("a"));
  CHECK(r.code == 2);
  CHECK(has(r.err, "capitol"));
  CHECK(has(r.err, "a.json"));                                   // the file is named
  r = run(p.config("b.json", good + R"(, "sweep": {"grid": {"x": [1]}}})"), p.out("b"));
  CHECK(r.code == 2);
  CHECK(has(r.err, "backtestx sweep"));
  r = run(p.config("c.json", R"({"data": {"files": ["AAA"]}, "benchmark": null, "strategy": {"name": "sma_crossover", "params": {"fast": 30, "slow": 10}}})"), p.out("c"));
  CHECK(r.code == 2);                                            // bad strategy params are caught before any data is read
  r = run((p.root / "configs" / "missing.json").string(), p.out("d"));
  CHECK(r.code == 2);
  r = run(p.config("e.json", good + "}"), p.out("e"));           // fine
  CHECK(r.code == 0);
  p.raw("BAD.csv", "Date,Open,High,Low,Close,Adj Close,Volume\n2021-01-04,100,90,95,95,95,10\n");     // high < low
  r = run(p.config("f.json", R"({"data": {"files": ["BAD"]}, "benchmark": null, "strategy": {"name": "buy_and_hold"}})"), p.out("f"));
  CHECK(r.code == 1);
  CHECK(has(r.err, "BAD.csv"));
  CHECK(has(r.err, "line 2"));
  // usage
  CHECK(run_cmd(cmd_run, {}).code == 2);
  CHECK(run_cmd(cmd_run, {"--config", "x.json"}).code == 2);
  CHECK(run_cmd(cmd_run, {"--bogus", "x"}).code == 2);
  CHECK(run_cmd(cmd_sweep, {"--config", "x.json", "--out", "o", "--threads", "lots"}).code == 2);
  CHECK(run_cmd(cmd_sweep, {"--config", "x.json", "--out", "o", "--threads", "5000"}).code == 2);
  // `sweep` on a config without a sweep block
  r = run_cmd(cmd_sweep, {"--config", p.config("g.json", good + "}"), "--out", p.out("g")});
  CHECK(r.code == 2);
  CHECK(has(r.err, "sweep"));
}

TEST_CASE("cli_main dispatch") {
  std::ostringstream o, e;
  CHECK(cli_main({"--version"}, o, e) == 0);
  CHECK(o.str() == std::string("backtestx ") + version() + "\n");
  std::ostringstream o2, e2;
  CHECK(cli_main({}, o2, e2) == 2);
  CHECK(has(e2.str(), "backtestx run"));
  std::ostringstream o3, e3;
  CHECK(cli_main({"frobnicate"}, o3, e3) == 2);
  CHECK(has(e3.str(), "unknown command"));
  std::ostringstream o4, e4;
  CHECK(cli_main({"--help"}, o4, e4) == 0);
  CHECK(has(o4.str(), "sweep"));
}

TEST_CASE("sweep via the CLI: --threads 1, 2 and 6 write byte-identical results; best/ is a complete run folder") {
  const Project p("clisweep");
  p.csv("AAA.csv", gbm(4, 700, 100));
  p.csv("NIFTYBEES.NS.csv", gbm(5, 700, 250, 0.15));
  const std::string cfg = p.config("s.json", R"({"data": {"files": ["AAA"]}, "period": {"start": "2021-09-01"},
      "strategy": {"name": "sma_crossover", "params": {"fast": 10, "slow": 30}},
      "sweep": {"grid": {"fast": [5, 8, 12, 40], "slow": {"start": 20, "stop": 50, "step": 10}}, "min_trades": 2,
                "walk_forward": {"in_sample_fraction": 0.65}}})");
  std::string first_csv, first_summary;
  for (const char* threads : {"1", "2", "6"}) {
    const std::string out = p.out((std::string("t") + threads).c_str());
    const Result r = run_cmd(cmd_sweep, {"--config", cfg, "--out", out, "--threads", threads});
    INFO(r.err);
    REQUIRE(r.code == 0);
    CHECK(has(r.out, "IN-SAMPLE"));
    CHECK(has(r.out, "out-of-sample"));
    const std::string csv = slurp(fs::path(out) / "sweep.csv");
    std::string summary = slurp(fs::path(out) / "sweep_summary.json");
    // threads_used legitimately differs; everything else must be identical
    json sj = json::parse(summary);
    sj.erase("threads_used");
    summary = sj.dump();
    if (first_csv.empty()) {
      first_csv = csv;
      first_summary = summary;
    }
    CHECK(csv == first_csv);
    CHECK(summary == first_summary);
    for (const char* f : {"equity.csv", "trades.csv", "fills.csv", "benchmark.csv", "summary.json", "run.json"})
      CHECK(fs::exists(fs::path(out) / "best" / f));
  }
  // the 40-vs-20/30 grid points are invalid and are reported as such
  CHECK(has(first_csv, "invalid"));
  const json sj = json::parse(first_summary);
  CHECK(sj["counts"]["invalid"] == 3);                           // (40,20) (40,30) (40,40)
  CHECK(sj["counts"]["ok"] == 13);
}

TEST_CASE("the shipped sample configs run (offline, on data/sample)") {
  const std::string dir = BT_CONFIG_DIR;
  const Project p("shipped");
  for (const char* name : {"sma.json", "momentum.json"}) {
    CAPTURE(name);
    const Result r = run((fs::path(dir) / name).string(), p.out(name));
    INFO(r.err);
    CHECK(r.code == 0);
    CHECK(fs::exists(fs::path(p.out(name)) / "summary.json"));
  }
  const Result s = run_cmd(cmd_sweep, {"--config", (fs::path(dir) / "sma_sweep.json").string(), "--out", p.out("sweep")});
  INFO(s.err);
  CHECK(s.code == 0);
  CHECK(fs::exists(fs::path(p.out("sweep")) / "sweep.csv"));
  CHECK(fs::exists(fs::path(p.out("sweep")) / "best" / "summary.json"));
}
