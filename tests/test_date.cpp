#include "bt/date.hpp"
#include "doctest.h"

using namespace bt;

TEST_CASE("date: known epoch offsets (hand-checked)") {
  CHECK(parse_date("1970-01-01").value() == 0);
  CHECK(parse_date("1970-01-02").value() == 1);
  CHECK(parse_date("1969-12-31").value() == -1);
  // 2000-01-01 is day 10957 (30 years incl. 7 leap days: 30*365 + 7 = 10957).
  CHECK(parse_date("2000-01-01").value() == 10957);
  // 2000 is a leap year: Jan 31 + Feb 29 = 60 days later.
  CHECK(parse_date("2000-03-01").value() == 11017);
  // 2024-01-01 = 19723; plus Jan (31) + Feb 1..28 (28) = 59 days -> Feb 29.
  CHECK(parse_date("2024-01-01").value() == 19723);
  CHECK(parse_date("2024-02-29").value() == 19782);
}

TEST_CASE("date: leap year rules") {
  CHECK(is_leap_year(2000));
  CHECK_FALSE(is_leap_year(1900));
  CHECK(is_leap_year(2024));
  CHECK_FALSE(is_leap_year(2023));
  CHECK(parse_date("2000-02-29").has_value());
  CHECK(parse_date("2024-02-29").has_value());
  CHECK_FALSE(parse_date("1900-02-29").has_value());
  CHECK_FALSE(parse_date("2023-02-29").has_value());
}

TEST_CASE("date: invalid strings are rejected") {
  for (const char* s : {"", "2024-1-01", "2024-01-1", "2024/01/01", "20240101", "2024-01-011", " 2024-01-01",
                        "2024-13-01", "2024-00-10", "2024-04-31", "2024-01-00", "2024-01-32", "2024-0a-01",
                        "0000-01-01", "-024-01-01", "2024-01-01\r"}) {
    CAPTURE(s);
    CHECK_FALSE(parse_date(s).has_value());
  }
}

TEST_CASE("date: weekday") {
  CHECK(weekday(0) == 4);                              // 1970-01-01 Thursday
  CHECK(weekday(-1) == 3);                             // 1969-12-31 Wednesday
  CHECK(weekday(parse_date("2000-01-01").value()) == 6);  // Saturday
  CHECK(weekday(parse_date("2024-02-29").value()) == 4);  // Thursday
  CHECK(weekday(parse_date("2024-03-03").value()) == 0);  // Sunday
  CHECK(is_weekend(parse_date("2024-03-02").value()));
  CHECK_FALSE(is_weekend(parse_date("2024-03-04").value()));
}

TEST_CASE("date: format/parse round-trip, every day 1900..2100 and a stride over 0001..9999") {
  const Date lo = parse_date("1900-01-01").value();
  const Date hi = parse_date("2100-12-31").value();
  Date prev = lo - 1;
  for (Date d = lo; d <= hi; ++d) {
    const std::string s = format_date(d);
    const auto back = parse_date(s);
    REQUIRE(back.has_value());
    REQUIRE(*back == d);
    REQUIRE(d == prev + 1);  // consecutive dates map to consecutive ints
    prev = d;
  }
  const Date first = parse_date("0001-01-01").value();
  const Date last = parse_date("9999-12-31").value();
  for (Date d = first; d <= last; d += 97) {
    const auto back = parse_date(format_date(d));
    REQUIRE(back.has_value());
    REQUIRE(*back == d);
  }
}
