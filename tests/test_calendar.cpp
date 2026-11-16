#include <stdexcept>

#include "bt/calendar.hpp"
#include "doctest.h"

using namespace bt;

namespace {
Bar mk(Date d, double px) { return Bar{d, px, px + 1, px - 1, px, px, 100}; }
}  // namespace

// Calendar days are the ints 1..6. Symbol A trades 1,2,3,4,5. Symbol B is listed late (first bar on
// day 3), has a one-day hole (no day 4) and trades on day 6 which A does not.
//   day:  1  2  3  4  5  6
//   A:    a1 a2 a3 a4 a5 -
//   B:    -  -  b3 -  b5 b6
static AlignedData sample() {
  return align_calendar({
      {"A", {mk(1, 10), mk(2, 11), mk(3, 12), mk(4, 13), mk(5, 14)}},
      {"B", {mk(3, 50), mk(5, 52), mk(6, 53)}},
  });
}

TEST_CASE("calendar: union of dates, symbols keep their order") {
  const auto d = sample();
  REQUIRE(d.num_days() == 6);
  REQUIRE(d.num_symbols() == 2);
  for (std::size_t t = 0; t < 6; ++t) CHECK(d.date(t) == static_cast<Date>(t + 1));
  CHECK(d.symbol(0) == "A");
  CHECK(d.symbol(1) == "B");
  CHECK(d.symbol_index("B").value() == 1);
  CHECK_FALSE(d.symbol_index("Z").has_value());
}

TEST_CASE("calendar: missing bars are nullptr, never invented") {
  const auto d = sample();
  // Leading gap (B not yet listed) and mid-series hole (B on day 4) and early end (A on day 6).
  CHECK(d.bar(1, 0) == nullptr);
  CHECK(d.bar(1, 1) == nullptr);
  CHECK(d.bar(1, 3) == nullptr);
  CHECK(d.bar(0, 5) == nullptr);
  // Present bars are the real ones.
  REQUIRE(d.bar(0, 0) != nullptr);
  CHECK(d.bar(0, 0)->close == 10);
  REQUIRE(d.bar(1, 2) != nullptr);
  CHECK(d.bar(1, 2)->close == 50);
  REQUIRE(d.bar(1, 5) != nullptr);
  CHECK(d.bar(1, 5)->close == 53);
  // The underlying data contains only the real bars.
  CHECK(d.bars(1).size() == 3);
  CHECK(d.bars(0).size() == 5);
}

TEST_CASE("calendar: last_bar is an explicit, separate accessor") {
  const auto d = sample();
  CHECK(d.last_bar(1, 0) == nullptr);  // before B's first bar there is nothing to carry
  CHECK(d.last_bar(1, 1) == nullptr);
  REQUIRE(d.last_bar(1, 3) != nullptr);  // hole on day 4: carries b3 (a real bar from day 3)
  CHECK(d.last_bar(1, 3)->date == 3);
  CHECK(d.last_bar(1, 3)->close == 50);
  REQUIRE(d.last_bar(0, 5) != nullptr);  // A ended on day 5
  CHECK(d.last_bar(0, 5)->date == 5);
  // Where a bar exists, last_bar is that same bar.
  CHECK(d.last_bar(0, 2) == d.bar(0, 2));
  // ...and bar() itself is still null on the hole, i.e. no forward-fill leaked into it.
  CHECK(d.bar(1, 3) == nullptr);
}

TEST_CASE("calendar: preconditions throw") {
  CHECK_THROWS_AS(align_calendar({{"A", {mk(1, 1)}}, {"A", {mk(2, 1)}}}), std::invalid_argument);
  CHECK_THROWS_AS(align_calendar({{"", {mk(1, 1)}}}), std::invalid_argument);
  CHECK_THROWS_AS(align_calendar({{"A", {mk(2, 1), mk(1, 1)}}}), std::invalid_argument);
  CHECK_THROWS_AS(align_calendar({{"A", {mk(2, 1), mk(2, 1)}}}), std::invalid_argument);
}

TEST_CASE("calendar: empty inputs are fine") {
  CHECK(align_calendar({}).num_days() == 0);
  const auto d = align_calendar({{"A", {}}, {"B", {mk(7, 1)}}});
  CHECK(d.num_days() == 1);
  CHECK(d.bar(0, 0) == nullptr);
  CHECK(d.last_bar(0, 0) == nullptr);
  CHECK(d.bar(1, 0) != nullptr);
}

TEST_CASE("calendar: identical calendars align one-to-one") {
  const auto d = align_calendar({{"A", {mk(1, 1), mk(2, 2)}}, {"B", {mk(1, 3), mk(2, 4)}}});
  REQUIRE(d.num_days() == 2);
  for (std::size_t s = 0; s < 2; ++s)
    for (std::size_t t = 0; t < 2; ++t) CHECK(d.bar(s, t) == &d.bars(s)[t]);
}
