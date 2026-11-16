#include <filesystem>
#include <fstream>

#include "bt/run_config.hpp"
#include "doctest.h"

using namespace bt;
using nlohmann::json;
namespace fs = std::filesystem;

namespace {

const char* kMin = R"({"data": {"files": ["AAA"]}, "strategy": {"name": "buy_and_hold"}})";

RunConfig P(const std::string& text, const char* base = "/proj/configs") { return parse_run_config(json::parse(text), base, "n"); }

// Parses and returns the error text ("" if it parsed).
std::string err_of(const std::string& text) {
  try {
    P(text);
    return "";
  } catch (const ConfigError& e) {
    return e.what();
  }
}

bool has(const std::string& s, const char* part) { return s.find(part) != std::string::npos; }

}  // namespace

TEST_CASE("run config: a minimal config gets every default") {
  const RunConfig c = P(kMin);
  CHECK(c.name == "n");
  CHECK(c.prices == PriceBasis::Adjusted);                    // ADJUSTED is the default price basis
  CHECK(c.null_rows == NullPolicy::Fail);
  CHECK(c.max_daily_move == 0.5);
  CHECK_FALSE(c.start.has_value());
  CHECK_FALSE(c.end.has_value());
  CHECK(c.capital_rupees == 1'000'000.0);
  CHECK(c.costs.brokerage_flat_rupees == 0.0);                // brokerage Rs 0 default
  CHECK_FALSE(c.allow_short);
  CHECK(c.max_volume_participation == 0.0);
  CHECK(c.slippage.mode == SlippageConfig::Mode::None);
  CHECK_FALSE(c.allocation.has_value());
  CHECK(c.analytics.risk_free_annual == 0.065);
  CHECK(c.analytics.trading_days == 252);
  // paths resolve against the config directory; the default data dir is ../data
  CHECK(c.data_dir == "/proj/data");
  REQUIRE(c.files.size() == 1);
  CHECK(c.files[0].symbol == "AAA");
  CHECK(c.files[0].path == "/proj/data/AAA.csv");
  // the DEFAULT benchmark is NIFTYBEES.NS (not ^NSEI)
  CHECK(c.benchmark.mode == BenchmarkSpec::Mode::Default);
  CHECK(c.benchmark.symbol == "NIFTYBEES.NS");
  CHECK(c.benchmark.path == "/proj/data/NIFTYBEES.NS.csv");
  CHECK(c.benchmark.max_daily_move == 0.15);
}

TEST_CASE("run config: benchmark - absent = default, string / object = explicit, null = explicit opt-out") {
  CHECK(P(kMin).benchmark.mode == BenchmarkSpec::Mode::Default);
  {
    const RunConfig c = P(R"({"data": {"files": ["AAA"]}, "strategy": {"name": "buy_and_hold"}, "benchmark": "^NSEI"})");
    CHECK(c.benchmark.mode == BenchmarkSpec::Mode::Explicit);
    CHECK(c.benchmark.symbol == "^NSEI");
    CHECK(c.benchmark.path == "/proj/data/NSEI.csv");       // '^' is dropped from the file name
  }
  {
    const RunConfig c = P(R"({"data": {"files": ["AAA"]}, "strategy": {"name": "buy_and_hold"},
                              "benchmark": {"symbol": "IDX", "path": "x/idx.csv", "max_daily_move": 0.3}})");
    CHECK(c.benchmark.symbol == "IDX");
    CHECK(c.benchmark.path == "/proj/configs/x/idx.csv");   // explicit paths are relative to the config file
    CHECK(c.benchmark.max_daily_move == 0.3);
  }
  CHECK(P(R"({"data": {"files": ["AAA"]}, "strategy": {"name": "buy_and_hold"}, "benchmark": null})").benchmark.mode ==
        BenchmarkSpec::Mode::None);
  CHECK(has(err_of(R"({"data": {"files": ["AAA"]}, "strategy": {"name": "buy_and_hold"}, "benchmark": 5})"), "benchmark:"));
  CHECK(has(err_of(R"({"data": {"files": ["AAA"]}, "strategy": {"name": "buy_and_hold"}, "benchmark": {"path": "p"}})"), "benchmark.symbol"));
  CHECK(has(err_of(R"({"data": {"files": ["AAA"]}, "strategy": {"name": "buy_and_hold"}, "benchmark": {"symbol": "I", "bogus": 1}})"),
            "benchmark.bogus"));
}

TEST_CASE("run config: unknown keys and wrong types are errors that name the key path") {
  CHECK(has(err_of(R"({"data": {"files": ["AAA"]}, "strategy": {"name": "buy_and_hold"}, "capitol": 5})"), "capitol"));
  CHECK(has(err_of(R"({"data": {"files": ["AAA"], "frequency": "daily"}, "strategy": {"name": "buy_and_hold"}})"), "data.frequency"));
  CHECK(has(err_of(R"({"data": {"files": ["AAA"]}, "strategy": {"name": "buy_and_hold", "param": {}}})"), "strategy.param"));
  CHECK(has(err_of(R"({"data": {"files": ["AAA"]}, "strategy": {"name": "buy_and_hold"}, "period": {"begin": "2020-01-01"}})"), "period.begin"));
  CHECK(has(err_of(R"({"data": {"files": ["AAA"]}, "strategy": {"name": "buy_and_hold"}, "capital": "1000"})"), "capital"));
  CHECK(has(err_of(R"({"data": {"files": "AAA"}, "strategy": {"name": "buy_and_hold"}})"), "data.files"));
  CHECK(has(err_of(R"({"data": {"files": [5]}, "strategy": {"name": "buy_and_hold"}})"), "data.files[0]"));
  CHECK(has(err_of(R"({"data": {"files": ["AAA"]}, "strategy": {"name": "buy_and_hold"}, "period": {"start": 20200101}})"), "period.start"));
  CHECK(has(err_of(R"({"data": {"files": ["AAA"]}, "strategy": {"name": "buy_and_hold"}, "execution": {"allow_short": "yes"}})"), "execution.allow_short"));
  CHECK(has(err_of(R"({"data": {"files": ["AAA"]}, "strategy": {"name": "buy_and_hold"}, "analytics": {"trading_days": 252.5}})"), "analytics.trading_days"));
  CHECK(has(err_of("[1]"), "expected an object"));
  CHECK(has(err_of(R"({"strategy": {"name": "buy_and_hold"}})"), "data"));
  CHECK(has(err_of(R"({"data": {"files": ["AAA"]}})"), "strategy"));
  CHECK(has(err_of(R"({"data": {}, "strategy": {"name": "buy_and_hold"}})"), "data.files"));
  CHECK(has(err_of(R"({"data": {"files": []}, "strategy": {"name": "buy_and_hold"}})"), "data.files"));
}

TEST_CASE("run config: value checks") {
  auto with = [](const std::string& extra) { return std::string(R"({"data": {"files": ["AAA"]}, "strategy": {"name": "buy_and_hold"}, )") + extra + "}"; };
  CHECK(has(err_of(with(R"("capital": 0)")), "capital"));
  CHECK(has(err_of(with(R"("capital": -5)")), "capital"));
  CHECK(has(err_of(with(R"("period": {"start": "2020-13-01"})")), "period.start"));
  CHECK(has(err_of(with(R"("period": {"start": "2021-01-01", "end": "2020-01-01"})")), "after end"));
  CHECK(err_of(with(R"("period": {"start": "2020-01-01", "end": "2020-01-01"})")) == "");     // one day is allowed here
  CHECK(has(err_of(R"({"data": {"files": ["AAA"], "prices": "total_return"}, "strategy": {"name": "buy_and_hold"}})"), "data.prices"));
  CHECK(has(err_of(R"({"data": {"files": ["AAA"], "null_rows": "ignore"}, "strategy": {"name": "buy_and_hold"}})"), "data.null_rows"));
  CHECK(has(err_of(R"({"data": {"files": ["AAA", "AAA"]}, "strategy": {"name": "buy_and_hold"}})"), "listed twice"));
  CHECK(has(err_of(R"({"data": {"files": [{"path": "x.csv"}]}, "strategy": {"name": "buy_and_hold"}})"), "symbol"));
  CHECK(has(err_of(with(R"("execution": {"max_volume_participation": 1.5})")), "execution.max_volume_participation"));
  CHECK(has(err_of(with(R"("slippage": {"mode": "bps"})")), "slippage.mode"));
  CHECK(has(err_of(with(R"("analytics": {"risk_free_annual": 7})")), "analytics.risk_free_annual"));
  CHECK(has(err_of(with(R"("costs": {"stt_buy": 1})")), "stt_buy"));
  CHECK(has(err_of(with(R"("costs": {}, "costs_file": "c.json")")), "not both"));
  CHECK(has(err_of(with(R"("costs_file": "no_such_costs.json")")), "costs_file"));
}

TEST_CASE("run config: price basis, null rows and exclusions") {
  const RunConfig c = P(R"({"data": {"files": ["AAA"], "prices": "raw", "null_rows": "skip", "max_daily_move": 0.3,
                                     "exclude_dates": {"AAA": ["2019-12-20", "2019-12-19"], "NIFTYBEES.NS": ["2010-02-05"]}},
                            "strategy": {"name": "buy_and_hold"}})");
  CHECK(c.prices == PriceBasis::Raw);
  CHECK(c.null_rows == NullPolicy::Skip);
  CHECK(c.max_daily_move == 0.3);
  REQUIRE(c.exclude_dates.size() == 2);
  CHECK(c.exclude_dates[0].first == "AAA");                  // sorted by symbol
  CHECK(c.exclude_dates[0].second == std::vector<Date>{parse_date("2019-12-19").value(), parse_date("2019-12-20").value()});
  CHECK(c.exclude_dates[1].first == "NIFTYBEES.NS");         // the default benchmark is a valid target
  // an exclusion for a symbol that is neither traded nor the benchmark is a typo
  CHECK(has(err_of(R"({"data": {"files": ["AAA"], "exclude_dates": {"BBB": ["2019-12-19"]}}, "strategy": {"name": "buy_and_hold"}})"), "data.exclude_dates.BBB"));
  CHECK(has(err_of(R"({"data": {"files": ["AAA"], "exclude_dates": {"NIFTYBEES.NS": ["2019-12-19"]}}, "strategy": {"name": "buy_and_hold"}, "benchmark": null})"),
            "data.exclude_dates.NIFTYBEES.NS"));
  CHECK(has(err_of(R"({"data": {"files": ["AAA"], "exclude_dates": {"AAA": ["not-a-date"]}}, "strategy": {"name": "buy_and_hold"}})"), "data.exclude_dates.AAA[0]"));
  CHECK(has(err_of(R"({"data": {"files": ["AAA"], "exclude_dates": {"AAA": []}}, "strategy": {"name": "buy_and_hold"}})"), "non-empty"));
}

TEST_CASE("run config: files - strings, objects, path resolution") {
  const RunConfig c = P(R"({"data": {"dir": "d", "files": ["A", {"symbol": "B", "path": "sub/b.csv"}, {"symbol": "^X"}, {"symbol": "C", "path": "/abs/c.csv"}]},
                            "strategy": {"name": "buy_and_hold"}, "benchmark": null})");
  REQUIRE(c.files.size() == 4);
  CHECK(c.data_dir == "/proj/configs/d");
  CHECK(c.files[0].path == "/proj/configs/d/A.csv");
  CHECK(c.files[1].path == "/proj/configs/sub/b.csv");
  CHECK(c.files[2].path == "/proj/configs/d/X.csv");
  CHECK(c.files[3].path == "/abs/c.csv");
  CHECK(symbol_file_name("^NSEI") == "NSEI.csv");
  CHECK(symbol_file_name("RELIANCE.NS") == "RELIANCE.NS.csv");
}

TEST_CASE("run config: strategy name and parameters are validated at parse time") {
  CHECK(has(err_of(R"({"data": {"files": ["A"]}, "strategy": {"name": "magic"}})"), "unknown strategy"));
  CHECK(has(err_of(R"({"data": {"files": ["A"]}, "strategy": {"name": "sma_crossover", "params": {"fast": 50, "slow": 20}}})"), "fast"));
  CHECK(has(err_of(R"({"data": {"files": ["A"]}, "strategy": {"name": "sma_crossover", "params": {"fats": 5}}})"), "params.fats"));
  CHECK(has(err_of(R"({"data": {"files": ["A"]}, "strategy": {"name": "sma_crossover", "params": 5}})"), "strategy.params"));
  CHECK(has(err_of(R"({"data": {"files": ["A"]}, "strategy": {"name": ""}})"), "strategy.name"));
  const RunConfig c = P(R"({"data": {"files": ["A"]}, "strategy": {"name": "sma_crossover", "params": {"fast": 5, "slow": 20}}})");
  CHECK(c.params == json::parse(R"({"fast": 5, "slow": 20})"));
}

TEST_CASE("run config: allocation lives ONLY in sizing") {
  const RunConfig c = P(R"({"data": {"files": ["A"]}, "strategy": {"name": "buy_and_hold"}, "sizing": {"allocation": 0.8}})");
  REQUIRE(c.allocation.has_value());
  CHECK(*c.allocation == 0.8);
  CHECK_FALSE(c.params.contains("allocation"));                    // injected at run time, not stored in the params
  CHECK(to_json(c)["strategy"]["params"]["allocation"] == 0.8);    // ...but shown in the resolved config
  // in params -> error with a hint, whether or not sizing is also set
  const std::string e = err_of(R"({"data": {"files": ["A"]}, "strategy": {"name": "buy_and_hold", "params": {"allocation": 0.9}}})");
  CHECK(has(e, "strategy.params.allocation"));
  CHECK(has(e, "sizing"));
  CHECK(has(err_of(R"({"data": {"files": ["A"]}, "strategy": {"name": "buy_and_hold", "params": {"allocation": 0.9}}, "sizing": {"allocation": 0.8}})"),
            "strategy.params.allocation"));
  CHECK(has(err_of(R"({"data": {"files": ["A"]}, "strategy": {"name": "buy_and_hold"}, "sizing": {"allocation": 0}})"), "sizing.allocation"));
  CHECK(has(err_of(R"({"data": {"files": ["A"]}, "strategy": {"name": "buy_and_hold"}, "sizing": {"allocation": 1.5}})"), "sizing.allocation"));
  CHECK(has(err_of(R"({"data": {"files": ["A"]}, "strategy": {"name": "buy_and_hold"}, "sizing": {"fraction": 0.5}})"), "sizing.fraction"));
}

TEST_CASE("run config: a sweep block is only accepted when asked for") {
  const std::string text = R"({"data": {"files": ["A"]}, "strategy": {"name": "buy_and_hold"}, "sweep": {"grid": {"x": [1]}}})";
  const std::string e = err_of(text);
  CHECK(has(e, "sweep"));
  CHECK(has(e, "backtestx sweep"));
  json block;
  const RunConfig c = parse_run_config(json::parse(text), "/proj/configs", "n", &block);
  CHECK(block == json::parse(R"({"grid": {"x": [1]}})"));
  (void)c;
}

TEST_CASE("run config: inline costs, costs_file, slippage, execution") {
  const RunConfig c = P(R"({"data": {"files": ["A"]}, "strategy": {"name": "buy_and_hold"},
                            "costs": {"brokerage": {"type": "flat", "flat_rupees": 20}},
                            "slippage": {"mode": "fixed_bps", "value": 5},
                            "execution": {"allow_short": true, "max_volume_participation": 0.1}})");
  CHECK(c.costs.brokerage_flat_rupees == 20.0);
  CHECK(c.slippage.mode == SlippageConfig::Mode::FixedBps);
  CHECK(c.slippage.value == 5.0);
  CHECK(c.allow_short);
  CHECK(c.max_volume_participation == 0.1);
  // costs_file resolves relative to the config directory
  const RunConfig f = parse_run_config(json::parse(R"({"data": {"files": ["A"]}, "strategy": {"name": "buy_and_hold"},
                                                        "costs_file": "costs_nse_delivery_flat20.json"})"), BT_CONFIG_DIR, "n");
  CHECK(f.costs.brokerage_flat_rupees == 20.0);
  CHECK(has(f.costs_source, "costs_nse_delivery_flat20.json"));
}

TEST_CASE("run config: load_run_config reads a file, names the file in errors, defaults the name to the file stem") {
  const fs::path dir = fs::temp_directory_path() / "bt_test_cfgfiles";
  fs::remove_all(dir);
  fs::create_directories(dir);
  auto write = [&](const char* name, const std::string& text) {
    std::ofstream(dir / name, std::ios::binary) << text;
    return (dir / name).string();
  };
  const std::string good = write("my_run.json", kMin);
  const RunConfig c = load_run_config(good);
  CHECK(c.name == "my_run");
  CHECK(c.base_dir == dir.string());

  const std::string bad = write("bad.json", R"({"data": {"files": ["AAA"]}, "strategy": {"name": "buy_and_hold"}, "oops": 1})");
  try {
    load_run_config(bad);
    FAIL("should have thrown");
  } catch (const ConfigError& e) {
    CHECK(has(e.what(), "bad.json"));
    CHECK(has(e.what(), "oops"));
  }
  const std::string junk = write("junk.json", "{not json");
  CHECK_THROWS_AS(load_run_config(junk), ConfigError);
  CHECK_THROWS_AS(load_run_config((dir / "missing.json").string()), ConfigError);
  fs::remove_all(dir);
}

TEST_CASE("run config: the resolved JSON shows every default that was applied") {
  const json j = to_json(P(kMin));
  CHECK(j["data"]["prices"] == "adjusted");
  CHECK(j["data"]["null_rows"] == "fail");
  CHECK(j["benchmark"]["mode"] == "default");
  CHECK(j["benchmark"]["symbol"] == "NIFTYBEES.NS");
  CHECK(j["capital"] == 1'000'000.0);
  CHECK(j["costs"]["brokerage"]["flat_rupees"] == 0.0);
  CHECK(j["period"]["start"].is_null());
  CHECK(j["analytics"]["risk_free_annual"] == 0.065);
}
