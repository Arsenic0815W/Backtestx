#pragma once
#include <iosfwd>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "bt/bar.hpp"

namespace bt {

// What to do with a "null row": a row whose date is valid but at least one of the six value fields
// (Open..Volume) is empty, the text "null", or the text "nan" (case-insensitive). Yahoo emits these
// for days it has a timestamp but no trade data (holidays, suspended stocks).
enum class NullPolicy {
  Fail,  // hard error NullField (default: strict)
  Skip,  // drop the row, record a LoadWarning
};

struct LoadOptions {
  NullPolicy null_policy = NullPolicy::Fail;
};

struct LoadWarning {
  int line;  // 1-based file line (header is line 1)
  std::string message;
};

struct LoadError {
  DataError code;
  int line;  // 1-based file line; 0 when not tied to a line (FileOpen)
  std::string message;
};

struct LoadResult {
  std::vector<Bar> bars;  // empty if error
  std::vector<LoadWarning> warnings;
  std::optional<LoadError> error;
  bool ok() const { return !error.has_value(); }
};

// Yahoo-style daily CSV: header "Date,Open,High,Low,Close,Adj Close,Volume".
// Accepts LF or CRLF, a UTF-8 BOM, blank lines, and spaces around fields.
// Stops at the first hard error. See bar.hpp for the validation order; date order is checked last,
// against the previous *accepted* row (skipped null rows never take part).
LoadResult load_csv(std::istream& in, const LoadOptions& opt = {});
LoadResult load_csv_string(std::string_view text, const LoadOptions& opt = {});
LoadResult load_csv_file(const std::string& path, const LoadOptions& opt = {});

// "source: line N: message"
std::string format_error(const std::string& source, const LoadError& e);

// Writes header + rows with LF line endings. Doubles use shortest round-trip text, so
// load_csv(write_csv(x)) == x exactly.
void write_csv(std::ostream& out, const std::vector<Bar>& bars);
std::string to_csv_string(const std::vector<Bar>& bars);

}  // namespace bt
