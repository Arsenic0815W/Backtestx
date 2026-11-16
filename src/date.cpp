#include "bt/date.hpp"

#include <cstdio>

namespace bt {

CivilDate civil_from_days(Date z) {
  z += 719468;
  const int era = (z >= 0 ? z : z - 146096) / 146097;
  const unsigned doe = static_cast<unsigned>(z - era * 146097);
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const int y = static_cast<int>(yoe) + era * 400;
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned mp = (5 * doy + 2) / 153;
  const unsigned d = doy - (153 * mp + 2) / 5 + 1;
  const unsigned m = mp < 10 ? mp + 3 : mp - 9;
  return {y + (m <= 2 ? 1 : 0), static_cast<int>(m), static_cast<int>(d)};
}

namespace {
bool digits(std::string_view s, std::size_t from, std::size_t to, int& out) {
  int v = 0;
  for (std::size_t i = from; i < to; ++i) {
    if (s[i] < '0' || s[i] > '9') return false;
    v = v * 10 + (s[i] - '0');
  }
  out = v;
  return true;
}
}  // namespace

std::optional<Date> parse_date(std::string_view s) {
  if (s.size() != 10 || s[4] != '-' || s[7] != '-') return std::nullopt;
  int y = 0, m = 0, d = 0;
  if (!digits(s, 0, 4, y) || !digits(s, 5, 7, m) || !digits(s, 8, 10, d)) return std::nullopt;
  if (y < 1 || m < 1 || m > 12 || d < 1 || d > days_in_month(y, m)) return std::nullopt;
  return days_from_civil(y, static_cast<unsigned>(m), static_cast<unsigned>(d));
}

std::string format_date(Date d) {
  const CivilDate c = civil_from_days(d);
  char buf[16];
  std::snprintf(buf, sizeof buf, "%04d-%02d-%02d", c.year, c.month, c.day);
  return buf;
}

int weekday(Date d) {
  // (d % 7) is negative for dates before 1970; +7 normalises, +4 maps day 0 (a Thursday) to 4.
  return static_cast<int>(((d % 7) + 7 + 4) % 7);
}

}  // namespace bt
