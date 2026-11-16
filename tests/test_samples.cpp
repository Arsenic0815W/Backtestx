#include <string>
#include <vector>

#include "bt/calendar.hpp"
#include "bt/csv.hpp"
#include "doctest.h"

using namespace bt;

// The shipped sample files (synthetic, see data/sample/README.md) are validated by the same strict
// loader on every test run. Expected counts/ranges were cross-checked with Python's datetime:
//   A, B: 500 weekdays from 2022-01-03 -> 2023-12-01;  C: 400 weekdays from 2022-07-01 -> 2024-01-11
//   union of all three calendars = 529 days; days on which all three trade = 371.

namespace {
std::string sample(const char* name) { return std::string(BT_SAMPLE_DIR) + "/" + name; }
}  // namespace

TEST_CASE("samples: every shipped file loads under the strict policy") {
  struct Expect { const char* file; std::size_t n; const char* first; const char* last; };
  const Expect cases[] = {
      {"SYNTH_A.csv", 500, "2022-01-03", "2023-12-01"},
      {"SYNTH_B.csv", 500, "2022-01-03", "2023-12-01"},
      {"SYNTH_C.csv", 400, "2022-07-01", "2024-01-11"},
  };
  for (const Expect& e : cases) {
    CAPTURE(e.file);
    const auto r = load_csv_file(sample(e.file));  // NullPolicy::Fail
    REQUIRE(r.ok());
    CHECK(r.warnings.empty());
    REQUIRE(r.bars.size() == e.n);
    CHECK(format_date(r.bars.front().date) == e.first);
    CHECK(format_date(r.bars.back().date) == e.last);
  }
}

TEST_CASE("samples: aligned calendar has the cross-checked size and C's leading gap") {
  std::vector<SymbolSeries> series;
  for (const char* name : {"SYNTH_A", "SYNTH_B", "SYNTH_C"}) {
    const auto r = load_csv_file(sample((std::string(name) + ".csv").c_str()));
    REQUIRE(r.ok());
    series.push_back({name, r.bars});
  }
  const auto d = align_calendar(std::move(series));
  REQUIRE(d.num_days() == 529);

  std::size_t all_three = 0;
  for (std::size_t t = 0; t < d.num_days(); ++t)
    if (d.bar(0, t) && d.bar(1, t) && d.bar(2, t)) ++all_three;
  CHECK(all_three == 371);

  CHECK(d.bar(2, 0) == nullptr);       // SYNTH_C not listed yet on 2022-01-03
  CHECK(d.bar(0, 0) != nullptr);
  CHECK(d.bar(0, d.num_days() - 1) == nullptr);  // A has ended by the last calendar day
  CHECK(d.last_bar(0, d.num_days() - 1) == &d.bars(0).back());
}
