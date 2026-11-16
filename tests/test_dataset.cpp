#include <filesystem>
#include <fstream>

#include "bt/analytics.hpp"
#include "bt/dataset.hpp"
#include "doctest.h"

using namespace bt;
namespace fs = std::filesystem;

namespace {
// A scratch CSV under the system temp dir (tests never depend on the working directory or on data/).
std::string write_temp(const char* name, const std::string& text) {
  const fs::path p = fs::temp_directory_path() / (std::string("bt_test_") + name);
  std::ofstream f(p, std::ios::binary);
  f << text;
  return p.string();
}
const char* kHeader = "Date,Open,High,Low,Close,Adj Close,Volume\n";
}  // namespace

TEST_CASE("dataset: the DEFAULT price basis is Adjusted") {
  CHECK(DatasetOptions{}.basis == PriceBasis::Adjusted);
}

TEST_CASE("dataset: adjusted by default, raw on request (hand-computed)") {
  // Close 200 with Adj Close 100 -> factor 0.5: O/H/L/C 198/204/196/200 become 99/102/98/100. Day 2 has factor 1.
  const std::string path = write_temp("split.csv", std::string(kHeader) +
                                                      "2024-01-01,198,204,196,200,100,10\n"
                                                      "2024-01-02,104,106,102,105,105,40\n");
  const std::vector<DataFile> files = {{"X", path}};

  const auto adj = load_dataset(files);  // defaults
  REQUIRE(adj.size() == 1);
  CHECK(adj[0].symbol == "X");
  CHECK(adj[0].bars[0].open == 99.0);
  CHECK(adj[0].bars[0].high == 102.0);
  CHECK(adj[0].bars[0].low == 98.0);
  CHECK(adj[0].bars[0].close == 100.0);
  CHECK(adj[0].bars[1].close == 105.0);

  DatasetOptions raw_opt;
  raw_opt.basis = PriceBasis::Raw;
  const auto raw = load_dataset(files, raw_opt);
  CHECK(raw[0].bars[0].close == 200.0);
  CHECK(raw[0].bars[0].adj_close == 100.0);
  CHECK(raw[0].bars[0].open == 198.0);
  fs::remove(path);
}

TEST_CASE("dataset: errors carry the file path and line; null policy is honoured") {
  const std::string bad = write_temp("bad.csv", std::string(kHeader) +
                                                   "2024-01-01,100,105,99,104,104,10\n"
                                                   "2024-01-02,100,98,99,99,99,10\n");  // high < low on line 3
  try {
    load_dataset({{"X", bad}});
    FAIL("should have thrown");
  } catch (const DatasetError& e) {
    const std::string m = e.what();
    CHECK(m.find("bt_test_bad.csv") != std::string::npos);
    CHECK(m.find("line 3") != std::string::npos);
  }
  fs::remove(bad);

  const std::string nulls = write_temp("nulls.csv", std::string(kHeader) +
                                                      "2024-01-01,100,105,99,104,104,10\n"
                                                      "2024-01-02,null,null,null,null,null,null\n"
                                                      "2024-01-03,104,106,103,105,105,10\n");
  CHECK_THROWS_AS(load_dataset({{"X", nulls}}), DatasetError);  // strict by default
  DatasetOptions skip;
  skip.load.null_policy = NullPolicy::Skip;
  CHECK(load_dataset({{"X", nulls}}, skip)[0].bars.size() == 2);
  fs::remove(nulls);

  CHECK_THROWS_AS(load_dataset({{"X", "no/such/file.csv"}}), DatasetError);
}

// ============================================================ exclusions and the implausible-move guard
namespace {
// Rows "date,o,h,l,c,adj,vol" with o=h=l=c=px for compact fixtures.
std::string rows(const std::vector<std::pair<const char*, double>>& v, double adj_factor = 1.0) {
  std::string t = kHeader;
  for (const auto& [d, px] : v) {
    char b[160];
    std::snprintf(b, sizeof b, "%s,%.6f,%.6f,%.6f,%.6f,%.6f,100\n", d, px, px, px, px, px * adj_factor);
    t += b;
  }
  return t;
}
}  // namespace

TEST_CASE("dataset: excluded dates are dropped (the file is untouched) and reported") {
  const std::string path = write_temp("excl.csv", rows({{"2024-01-01", 100}, {"2024-01-02", 101}, {"2024-01-03", 102}, {"2024-01-04", 103}}));
  std::vector<LoadedFileInfo> info;
  DataFile f{"X", path, {parse_date("2024-01-03").value(), parse_date("2024-01-03").value()}, 0.5};  // duplicate listed twice
  const auto s = load_dataset({f}, {}, &info);
  REQUIRE(s[0].bars.size() == 3);
  for (const Bar& b : s[0].bars) CHECK(b.date != parse_date("2024-01-03").value());
  REQUIRE(info.size() == 1);
  CHECK(info[0].bars == 3);
  CHECK(info[0].excluded == std::vector<Date>{parse_date("2024-01-03").value()});
  CHECK(format_date(info[0].first) == "2024-01-01");
  CHECK(format_date(info[0].last) == "2024-01-04");
  CHECK(info[0].largest_move > 0.0);   // the largest kept close-to-close move is reported
  // reading the file again still shows all four rows
  CHECK(load_dataset({{"X", path}})[0].bars.size() == 4);
  fs::remove(path);
}

TEST_CASE("dataset: an exclusion date that is not a bar in the file is an ERROR, not a no-op") {
  const std::string path = write_temp("excl_typo.csv", rows({{"2024-01-01", 100}, {"2024-01-02", 101}}));
  DataFile f{"X", path, {parse_date("2024-01-05").value()}, 0.5};
  try {
    load_dataset({f});
    FAIL("should have thrown");
  } catch (const DatasetError& e) {
    const std::string m = e.what();
    CHECK(m.find("2024-01-05") != std::string::npos);
    CHECK(m.find("not a bar") != std::string::npos);
  }
  // excluding every bar leaves nothing: also an error
  DataFile all{"X", path, {parse_date("2024-01-01").value(), parse_date("2024-01-02").value()}, 0.5};
  CHECK_THROWS_AS(load_dataset({all}), DatasetError);
  fs::remove(path);
}

TEST_CASE("dataset: the implausible-move guard fails loudly with the dates and prices") {
  // 129.25 -> 13.02 is a -89.9 % "move" (the shape of the NIFTYBEES glitch)
  const std::string path = write_temp("glitch.csv", rows({{"2024-01-01", 129.25}, {"2024-01-02", 13.02}, {"2024-01-03", 129.9}}));
  try {
    load_dataset({{"X", path}});
    FAIL("should have thrown");
  } catch (const DatasetError& e) {
    const std::string m = e.what();
    CHECK(m.find("2024-01-02") != std::string::npos);
    CHECK(m.find("-89.9%") != std::string::npos);
    CHECK(m.find("129.25") != std::string::npos);
    CHECK(m.find("exclude_dates") != std::string::npos);
  }
  DataFile off{"X", path, {}, 1000.0};   // guard effectively off
  CHECK_NOTHROW(load_dataset({off}));
  // a genuine 12 % move is fine under the default 0.5 but trips a 0.1 guard (just under the move)
  const std::string real = write_temp("real_move.csv", rows({{"2024-01-01", 100}, {"2024-01-02", 112}}));
  CHECK_NOTHROW(load_dataset({{"X", real}}));
  DataFile tight{"X", real, {}, 0.10};
  CHECK_THROWS_AS(load_dataset({tight}), DatasetError);
  // the move is 112/100 - 1 = 0.12 (a hair above in binary floating point): a threshold just above passes, just below fails
  DataFile above{"X", real, {}, 0.1201};
  CHECK_NOTHROW(load_dataset({above}));
  DataFile below{"X", real, {}, 0.1199};
  CHECK_THROWS_AS(load_dataset({below}), DatasetError);
  fs::remove(path);
  fs::remove(real);
}

TEST_CASE("dataset: excluding the glitch bars first lets the guard pass (order: exclude, then guard)") {
  const std::string path = write_temp("glitch2.csv", rows({{"2024-01-01", 129.25}, {"2024-01-02", 13.02}, {"2024-01-03", 13.03}, {"2024-01-04", 129.9}}));
  CHECK_THROWS_AS(load_dataset({{"X", path}}), DatasetError);
  DataFile f{"X", path, {parse_date("2024-01-02").value(), parse_date("2024-01-03").value()}, 0.5};
  const auto s = load_dataset({f});
  CHECK(s[0].bars.size() == 2);
  fs::remove(path);
}

TEST_CASE("dataset: exclusion happens before price adjustment (the remaining bars adjust exactly as without it)") {
  // adj/close = 0.5 on every row: adjusted close is half the close; excluding a row must not change the other bars
  const std::string path = write_temp("excl_adj.csv", rows({{"2024-01-01", 100}, {"2024-01-02", 104}, {"2024-01-03", 108}}, 0.5));
  const auto full = load_dataset({{"X", path}});
  DataFile f{"X", path, {parse_date("2024-01-02").value()}, 0.5};
  const auto cut = load_dataset({f});
  REQUIRE(cut[0].bars.size() == 2);
  CHECK(cut[0].bars[0] == full[0].bars[0]);
  CHECK(cut[0].bars[1] == full[0].bars[2]);
  CHECK(cut[0].bars[0].close == 50.0);
  fs::remove(path);
}

TEST_CASE("dataset: an excluded benchmark date becomes a GAP that the pairing rule handles (beta stays exactly 2)") {
  // Benchmark file with a corrupt value on day 2 (1000 instead of ~101). Strategy equity is exactly 2x the benchmark return on
  // every interval between the remaining common dates (the Phase 4 hand case). Unexcluded, the guard refuses the file.
  const std::string path = write_temp("bench_gap.csv", rows({{"2024-01-01", 100.0}, {"2024-01-02", 101.0}, {"2024-01-03", 1000.0},
                                                               {"2024-01-04", 103.02}, {"2024-01-05", 101.9898}, {"2024-01-06", 104.539545}}));
  DataFile guarded{"IDX", path, {}, 0.15};
  CHECK_THROWS_AS(load_dataset({guarded}), DatasetError);
  DataFile excl{"IDX", path, {parse_date("2024-01-03").value()}, 0.15};
  const SymbolSeries b = load_dataset({excl})[0];
  const BenchmarkSeries bench = benchmark_from_bars("IDX", b.bars);
  const Date d0 = parse_date("2024-01-01").value();
  std::vector<DailyRecord> curve;
  const Paise eq[] = {10'000'000, 10'200'000, 10'300'000, 10'608'000, 10'395'840, 10'915'632};
  for (int i = 0; i < 6; ++i) curve.push_back({d0 + i, eq[i], 0, eq[i]});
  const Analytics a = analyze(curve, {}, {}, AnalyticsConfig{0.0, 252}, &bench);
  REQUIRE(a.benchmark.has_value());
  CHECK(a.benchmark->n_common_dates == 5);
  CHECK(a.benchmark->n_curve_dates_without_benchmark == 1);   // the excluded day
  REQUIRE(a.benchmark->beta.has_value());
  CHECK(*a.benchmark->beta == doctest::Approx(2.0).epsilon(1e-9));
  fs::remove(path);
}
