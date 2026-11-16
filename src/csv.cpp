#include "bt/csv.hpp"

#include <charconv>
#include <cmath>
#include <fstream>
#include <istream>
#include <sstream>

namespace bt {
namespace {

constexpr std::string_view kHeader = "Date,Open,High,Low,Close,Adj Close,Volume";
constexpr const char* kFieldNames[7] = {"date", "open", "high", "low", "close", "adj_close", "volume"};

std::string_view trim(std::string_view s) {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
  return s;
}

bool iequals(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  for (std::size_t i = 0; i < a.size(); ++i) {
    char x = a[i], y = b[i];
    if (x >= 'A' && x <= 'Z') x = static_cast<char>(x - 'A' + 'a');
    if (y >= 'A' && y <= 'Z') y = static_cast<char>(y - 'A' + 'a');
    if (x != y) return false;
  }
  return true;
}

bool is_null_token(std::string_view s) { return s.empty() || iequals(s, "null") || iequals(s, "nan"); }

// Locale-independent; the whole field must be consumed. "inf" parses (caller rejects non-finite).
std::optional<double> parse_double(std::string_view s) {
  if (s.empty()) return std::nullopt;
  double v = 0;
  const auto r = std::from_chars(s.data(), s.data() + s.size(), v);
  if (r.ec != std::errc{} || r.ptr != s.data() + s.size()) return std::nullopt;
  return v;
}

std::optional<std::int64_t> parse_int64(std::string_view s) {
  if (s.empty()) return std::nullopt;
  std::int64_t v = 0;
  const auto r = std::from_chars(s.data(), s.data() + s.size(), v);
  if (r.ec != std::errc{} || r.ptr != s.data() + s.size()) return std::nullopt;
  return v;
}

std::string fmt(double v) {
  char buf[64];
  const auto r = std::to_chars(buf, buf + sizeof buf, v);
  return std::string(buf, r.ptr);
}

std::string at_line(int line, const std::string& msg) { return "line " + std::to_string(line) + ": " + msg; }

}  // namespace

LoadResult load_csv(std::istream& in, const LoadOptions& opt) {
  LoadResult res;
  auto fail = [&](DataError code, int line, const std::string& msg) {
    res.bars.clear();
    res.error = LoadError{code, line, at_line(line, msg)};
    return res;
  };

  std::string line;
  int line_no = 0;

  // ---- header (must be line 1) ----
  if (!std::getline(in, line)) return fail(DataError::BadHeader, 1, "empty file (no header)");
  line_no = 1;
  if (line.size() >= 3 && line.compare(0, 3, "\xEF\xBB\xBF") == 0) line.erase(0, 3);
  if (!line.empty() && line.back() == '\r') line.pop_back();
  if (trim(line) != kHeader)
    return fail(DataError::BadHeader, 1, "header must be exactly '" + std::string(kHeader) + "'");

  // ---- rows ----
  while (std::getline(in, line)) {
    ++line_no;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (trim(line).empty()) continue;  // blank lines are ignored but still counted

    std::string_view f[7];
    std::size_t n = 0, start = 0;
    const std::string_view row(line);
    bool too_many = false;
    for (std::size_t i = 0; i <= row.size(); ++i) {
      if (i == row.size() || row[i] == ',') {
        if (n == 7) {
          too_many = true;
          break;
        }
        f[n++] = trim(row.substr(start, i - start));
        start = i + 1;
      }
    }
    if (too_many || n != 7)
      return fail(DataError::BadFieldCount, line_no,
                  "expected 7 fields, got " + (too_many ? std::string("more") : std::to_string(n)));

    const auto date = parse_date(f[0]);
    if (!date)
      return fail(DataError::BadDate, line_no, "invalid date '" + std::string(f[0]) + "' (want YYYY-MM-DD)");

    bool has_null = false;
    for (int i = 1; i < 7; ++i)
      if (is_null_token(f[i])) {
        has_null = true;
        break;
      }
    if (has_null) {
      if (opt.null_policy == NullPolicy::Fail)
        return fail(DataError::NullField, line_no, "null/empty/NaN field on " + std::string(f[0]));
      res.warnings.push_back({line_no, at_line(line_no, "skipped null row " + std::string(f[0]))});
      continue;
    }

    Bar b;
    b.date = *date;
    double* px[5] = {&b.open, &b.high, &b.low, &b.close, &b.adj_close};
    for (int i = 0; i < 5; ++i) {
      const auto v = parse_double(f[i + 1]);
      if (!v || !std::isfinite(*v))
        return fail(DataError::BadNumber, line_no,
                    std::string(kFieldNames[i + 1]) + " '" + std::string(f[i + 1]) + "' is not a finite number");
      *px[i] = *v;
    }
    const auto vol = parse_int64(f[6]);
    if (!vol) return fail(DataError::BadNumber, line_no, "volume '" + std::string(f[6]) + "' is not an integer");
    b.volume = *vol;

    if (const auto e = check_bar(b)) {
      std::string msg;
      switch (*e) {
        case DataError::NonPositivePrice: msg = "prices must be > 0"; break;
        case DataError::NegativeVolume: msg = "volume must be >= 0"; break;
        case DataError::HighBelowLow: msg = "high " + fmt(b.high) + " < low " + fmt(b.low); break;
        case DataError::OpenOutsideRange:
          msg = "open " + fmt(b.open) + " outside [low " + fmt(b.low) + ", high " + fmt(b.high) + "]";
          break;
        case DataError::CloseOutsideRange:
          msg = "close " + fmt(b.close) + " outside [low " + fmt(b.low) + ", high " + fmt(b.high) + "]";
          break;
        default: msg = to_string(*e); break;
      }
      return fail(*e, line_no, msg);
    }

    if (!res.bars.empty()) {
      const Date prev = res.bars.back().date;
      if (b.date == prev) return fail(DataError::DuplicateDate, line_no, "duplicate date " + format_date(b.date));
      if (b.date < prev)
        return fail(DataError::NonIncreasingDate, line_no,
                    "date " + format_date(b.date) + " is not after previous " + format_date(prev));
    }
    res.bars.push_back(b);
  }

  if (res.bars.empty()) return fail(DataError::NoData, line_no, "no usable data rows");
  return res;
}

LoadResult load_csv_string(std::string_view text, const LoadOptions& opt) {
  std::istringstream in{std::string(text)};
  return load_csv(in, opt);
}

LoadResult load_csv_file(const std::string& path, const LoadOptions& opt) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    LoadResult r;
    r.error = LoadError{DataError::FileOpen, 0, "cannot open file"};
    return r;
  }
  return load_csv(in, opt);
}

std::string format_error(const std::string& source, const LoadError& e) {
  return source + ": " + e.message;  // message already carries "line N: " when tied to a line
}

void write_csv(std::ostream& out, const std::vector<Bar>& bars) {
  out << kHeader << '\n';
  for (const Bar& b : bars)
    out << format_date(b.date) << ',' << fmt(b.open) << ',' << fmt(b.high) << ',' << fmt(b.low) << ','
        << fmt(b.close) << ',' << fmt(b.adj_close) << ',' << b.volume << '\n';
}

std::string to_csv_string(const std::vector<Bar>& bars) {
  std::ostringstream os;
  write_csv(os, bars);
  return os.str();
}

}  // namespace bt
