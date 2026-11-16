// Phase 6: the report's numbers (hand-computed), the embedding safety, and end-to-end report files from the sample data.
#include <filesystem>
#include <fstream>
#include <sstream>

#include "bt/commands.hpp"
#include "bt/date.hpp"
#include "bt/report.hpp"
#include "bt/report_assets.hpp"
#include "doctest.h"
#include "json.hpp"

using namespace bt;
using nlohmann::json;
namespace fs = std::filesystem;

namespace {

DailyRecord rec(const char* d, Paise equity) {
  DailyRecord r;
  r.date = parse_date(d).value();
  r.equity = equity;
  r.positions_value = equity;
  return r;
}

std::string slurp(const fs::path& p) {
  std::ifstream f(p, std::ios::binary);
  std::ostringstream s;
  s << f.rdbuf();
  return s.str();
}

struct Cli {
  int code;
  std::string out, err;
};
Cli cli(const std::vector<std::string>& a) {
  std::ostringstream o, e;
  const int c = cli_main(a, o, e);
  return {c, o.str(), e.str()};
}

}  // namespace

TEST_CASE("monthly returns: base is the previous month's last record, the first month starts at the first record") {
  // Jan: 100 -> 110 (first month, base = first record 100). Feb: last 99 vs Jan last 110 -> -10 %. Mar: one record 99 -> 108.9 (+10 %).
  const std::vector<DailyRecord> c = {rec("2024-01-02", 100), rec("2024-01-31", 110), rec("2024-02-01", 121), rec("2024-02-29", 99), rec("2024-03-28", 108)};
  const auto m = monthly_returns(c);
  REQUIRE(m.size() == 3);
  CHECK(m[0].year == 2024);
  CHECK(m[0].month == 1);
  CHECK(m[0].ret.value() == doctest::Approx(0.10));
  CHECK(m[0].first);
  CHECK_FALSE(m[0].last);
  CHECK(format_date(m[0].from) == "2024-01-02");
  CHECK(m[1].ret.value() == doctest::Approx(99.0 / 110.0 - 1.0));
  CHECK(format_date(m[1].from) == "2024-01-31");
  CHECK(format_date(m[1].to) == "2024-02-29");
  CHECK(m[2].ret.value() == doctest::Approx(108.0 / 99.0 - 1.0));
  CHECK(m[2].last);
}

TEST_CASE("yearly return equals the compounded monthly returns") {
  const std::vector<DailyRecord> c = {rec("2023-11-30", 100), rec("2023-12-29", 120), rec("2024-01-31", 90), rec("2024-12-31", 150)};
  const auto m = monthly_returns(c);
  const auto y = yearly_returns(c);
  REQUIRE(y.size() == 2);
  CHECK(y[0].ret.value() == doctest::Approx(0.2));
  CHECK(y[1].ret.value() == doctest::Approx(150.0 / 120.0 - 1.0));
  double compounded = 1.0;
  for (const auto& mm : m)
    if (mm.year == 2024) compounded *= 1.0 + mm.ret.value();
  CHECK(compounded - 1.0 == doctest::Approx(y[1].ret.value()));
}

TEST_CASE("degenerate curves: empty, one record, a curve inside one month, non-positive base") {
  CHECK(monthly_returns({}).empty());
  const auto one = monthly_returns({rec("2024-05-06", 100)});
  REQUIRE(one.size() == 1);
  CHECK_FALSE(one[0].ret.has_value());
  CHECK(one[0].first);
  CHECK(one[0].last);
  const auto in_month = monthly_returns({rec("2024-05-06", 100), rec("2024-05-20", 103)});
  REQUIRE(in_month.size() == 1);
  CHECK(in_month[0].ret.value() == doctest::Approx(0.03));
  const auto neg = monthly_returns({rec("2024-05-06", -5), rec("2024-06-03", 10)});
  REQUIRE(neg.size() == 2);
  CHECK_FALSE(neg[1].ret.has_value());
}

TEST_CASE("drawdown series: distance below the running maximum; null when the peak is not positive") {
  const auto d = drawdown_series({100, 120, 90, 120, 130, 65});
  REQUIRE(d.size() == 6);
  CHECK(d[0].value() == doctest::Approx(0.0));
  CHECK(d[1].value() == doctest::Approx(0.0));
  CHECK(d[2].value() == doctest::Approx(-0.25));
  CHECK(d[3].value() == doctest::Approx(0.0));
  CHECK(d[5].value() == doctest::Approx(-0.5));
  const auto z = drawdown_series({0, 0, -3});
  CHECK_FALSE(z[0].has_value());
  CHECK_FALSE(z[2].has_value());
}

TEST_CASE("benchmark rebasing: common dates only, first common date equals the strategy equity") {
  const std::vector<DailyRecord> c = {rec("2024-01-02", 1000), rec("2024-01-03", 1010), rec("2024-01-04", 1020), rec("2024-01-05", 1000)};
  BenchmarkSeries b;
  b.name = "B";
  b.dates = {parse_date("2024-01-01").value(), parse_date("2024-01-03").value(), parse_date("2024-01-05").value(), parse_date("2024-01-08").value()};
  b.values = {50.0, 200.0, 220.0, 300.0};
  const auto r = rebase_benchmark(c, b);
  REQUIRE(r.has_value());
  REQUIRE(r->dates.size() == 2);
  CHECK(format_date(r->dates[0]) == "2024-01-03");
  CHECK(r->equity[0] == 1010);        // equals the strategy's equity on the first common date
  CHECK(r->equity[1] == 1111);        // 1010 * 220 / 200
  b.dates = {parse_date("2023-01-01").value()};
  b.values = {1.0};
  CHECK_FALSE(rebase_benchmark(c, b).has_value());
}

TEST_CASE("embedding: every '<' is escaped, output is ASCII, hostile titles are escaped in the HTML") {
  const json j = {{"name", "</script><!-- <b>\"&\xC3\xA9"}, {"n", 1}};
  const std::string s = script_safe_json(j);
  CHECK(s.find('<') == std::string::npos);
  CHECK(s.find("</script") == std::string::npos);
  for (unsigned char ch : s) CHECK(ch < 0x80);
  CHECK(json::parse(s) == j);  // round-trips exactly
  CHECK(html_escape("</script>&\"'") == "&lt;/script&gt;&amp;&quot;&#39;");

  json d = {{"kind", "run"}, {"title", "</title><script>alert(1)</script> @@BT_JS@@"}};
  const std::string html = render_report_html(d);
  CHECK(html.find("<script>alert(1)") == std::string::npos);
  CHECK(html.find("&lt;/title&gt;") != std::string::npos);
  // the marker text inside the data was NOT substituted
  CHECK(html.find("@@BT_JS@@") != std::string::npos);
  // exactly three script elements' worth of closers: data, code, and none injected
  std::size_t closers = 0;
  for (std::size_t p = 0; (p = html.find("</script>", p)) != std::string::npos; p += 9) ++closers;
  CHECK(closers == 2);
}

TEST_CASE("embedded assets are present and self-contained") {
  const std::string js = report_assets::report_js(), css = report_assets::report_css(), tpl = report_assets::report_template();
  CHECK(js.size() > 1000);
  CHECK(css.size() > 500);
  CHECK(tpl.find("@@BT_DATA@@") != std::string::npos);
  CHECK(js.find("</script") == std::string::npos);
  CHECK(js.find('\r') == std::string::npos);
  for (const std::string* a : {&js, &css, &tpl}) {
    CHECK(a->find("@import") == std::string::npos);
    CHECK(a->find("url(") == std::string::npos);
    CHECK(a->find("src=") == std::string::npos);
  }
}

TEST_CASE("end to end: run and sweep write report.html built from the folder; report command regenerates it identically") {
  const fs::path root = fs::temp_directory_path() / "bt_test_report";
  fs::remove_all(root);
  const std::string sample = BT_SAMPLE_DIR;
  const fs::path cfg = root / "cfg.json";
  fs::create_directories(root);
  {
    std::ofstream f(cfg);
    f << R"({"name":"</script>hostile & <b>","data":{"dir":")" << json(sample).dump().substr(1, json(sample).dump().size() - 2)
      << R"(","files":["SYNTH_A"]},"period":{"start":"2022-09-01","end":"2023-11-30"},"capital":1000000,)"
      << R"("strategy":{"name":"sma_crossover","params":{"fast":10,"slow":30}},"benchmark":"SYNTH_B"})";
  }
  const Cli r = cli({"run", "--config", cfg.string(), "--out", (root / "run").string()});
  INFO(r.err);
  REQUIRE(r.code == 0);
  const std::string html = slurp(root / "run" / "report.html");
  CHECK(html.size() > 10000);
  CHECK(html.find("Not investment advice. Past performance does not predict future results.") != std::string::npos);
  CHECK(html.find("<b>") == std::string::npos);
  CHECK(html.find(sample) == std::string::npos);  // no machine paths in the report
  const json d = build_run_report_data((root / "run").string());
  CHECK(d.at("kind") == "run");
  CHECK(d.at("benchmark").at("name") == "SYNTH_B");
  CHECK(d.at("equity").at("dates").size() == d.at("summary").at("equity").at("n_records").get<std::size_t>());
  CHECK(d.at("fees").at("total_paise") == d.at("summary").at("trades").at("total_fees_paise"));
  const json& pnl = d.at("pnl");
  CHECK(pnl.at("closed_trades_net_paise").get<std::int64_t>() + pnl.at("unrealised_paise").get<std::int64_t>() == pnl.at("total_paise").get<std::int64_t>());
  CHECK(pnl.at("total_paise") == d.at("summary").at("trades").at("net_pnl_paise"));
  const Cli again = cli({"report", "--dir", (root / "run").string()});
  REQUIRE(again.code == 0);
  CHECK(slurp(root / "run" / "report.html") == html);  // deterministic

  // a stale / missing file makes the report fail loudly
  fs::remove(root / "run" / "equity.csv");
  const Cli bad = cli({"report", "--dir", (root / "run").string()});
  CHECK(bad.code == 1);
  CHECK(bad.err.find("equity.csv") != std::string::npos);

  fs::remove_all(root);
}

TEST_CASE("sweep report: written next to sweep.csv, best/ gets its own report with the out-of-sample context") {
  const fs::path root = fs::temp_directory_path() / "bt_test_report_sweep";
  fs::remove_all(root);
  fs::create_directories(root);
  const std::string sample = BT_SAMPLE_DIR;
  const fs::path cfg = root / "cfg.json";
  {
    std::ofstream f(cfg);
    f << R"({"name":"sw","data":{"dir":")" << json(sample).dump().substr(1, json(sample).dump().size() - 2) << R"(","files":["SYNTH_A"]},)"
      << R"("period":{"start":"2022-09-01","end":"2023-11-30"},"capital":1000000,"strategy":{"name":"sma_crossover","params":{"fast":10,"slow":30}},)"
      << R"("benchmark":"SYNTH_B","sweep":{"grid":{"fast":[5,10,20],"slow":[30,40]},"metric":"sharpe","min_trades":1,"walk_forward":{"in_sample_fraction":0.6}}})";
  }
  const Cli r = cli({"sweep", "--config", cfg.string(), "--out", (root / "out").string(), "--threads", "1"});
  INFO(r.err);
  REQUIRE(r.code == 0);
  REQUIRE(fs::exists(root / "out" / "report.html"));
  REQUIRE(fs::exists(root / "out" / "best" / "report.html"));
  const json s = build_sweep_report_data((root / "out").string());
  CHECK(s.at("kind") == "sweep");
  CHECK(s.at("axes") == json::array({"fast", "slow"}));
  CHECK(s.at("rows").size() == 6);
  CHECK(s.at("best_report") == "best/report.html");
  const json b = build_run_report_data((root / "out" / "best").string());
  REQUIRE(b.contains("context"));
  CHECK(b.at("context").at("title") == "Out-of-sample run.");
  CHECK(b.at("context").at("link") == "../report.html");
  fs::remove_all(root);
}
