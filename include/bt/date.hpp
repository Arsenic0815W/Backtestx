#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace bt {

// A calendar date as days since 1970-01-01 (the Unix epoch), proleptic Gregorian.
// 1970-01-01 == 0, 1969-12-31 == -1. Plain int32 so comparing/sorting dates is integer comparison.
using Date = std::int32_t;

constexpr bool is_leap_year(int y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }

constexpr int days_in_month(int y, int m) {
  constexpr int kDays[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  return (m == 2 && is_leap_year(y)) ? 29 : kDays[m - 1];
}

// Howard Hinnant's days_from_civil. Caller guarantees a valid y/m/d (see parse_date for checking).
constexpr Date days_from_civil(int y, unsigned m, unsigned d) {
  y -= m <= 2 ? 1 : 0;
  const int era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(y - era * 400);
  const unsigned doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + static_cast<int>(doe) - 719468;
}

struct CivilDate {
  int year;
  int month;  // 1..12
  int day;    // 1..31
};

CivilDate civil_from_days(Date z);

// Strict "YYYY-MM-DD": exactly 10 chars, zero-padded, a real calendar day, year 0001..9999.
// Rejects 2023-02-29, 1900-02-29, month 13, "2024-1-01", trailing junk.
std::optional<Date> parse_date(std::string_view s);

std::string format_date(Date d);  // "YYYY-MM-DD"

// 0 = Sunday ... 6 = Saturday (1970-01-01 was a Thursday == 4).
int weekday(Date d);
inline bool is_weekend(Date d) {
  const int w = weekday(d);
  return w == 0 || w == 6;
}

}  // namespace bt
