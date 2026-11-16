#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "bt/date.hpp"

namespace bt {

using Symbol = std::string;

// One daily OHLCV bar. Prices are doubles (rupees); volume is shares and may be 0 (indices).
struct Bar {
  Date date = 0;
  double open = 0, high = 0, low = 0, close = 0;
  double adj_close = 0;
  std::int64_t volume = 0;
};

bool operator==(const Bar& a, const Bar& b);  // exact field-by-field comparison
inline bool operator!=(const Bar& a, const Bar& b) { return !(a == b); }

// Every way a bar / CSV row can be rejected. Tests assert on these codes, never on message text.
enum class DataError {
  FileOpen,           // file could not be opened
  BadHeader,          // first line is not exactly "Date,Open,High,Low,Close,Adj Close,Volume"
  BadFieldCount,      // row does not have exactly 7 comma-separated fields
  BadDate,            // not a strict, real YYYY-MM-DD date
  BadNumber,          // price/volume text is not a finite number (or volume not an integer)
  NullField,          // empty / "null" / "nan" field while NullPolicy::Fail
  NonPositivePrice,   // open/high/low/close/adj_close <= 0
  NegativeVolume,     // volume < 0 (zero is fine)
  HighBelowLow,       // high < low
  OpenOutsideRange,   // open not within [low, high]
  CloseOutsideRange,  // close not within [low, high]
  DuplicateDate,      // same date as the previous accepted row
  NonIncreasingDate,  // earlier date than the previous accepted row
  NoData,             // header present but no usable rows
};

const char* to_string(DataError e);

// Sanity-check a single bar (everything except date ordering). nullopt == valid.
// Check order: finite -> positive -> high>=low -> open in range -> close in range -> volume>=0.
std::optional<DataError> check_bar(const Bar& b);

struct SymbolSeries {
  Symbol symbol;
  std::vector<Bar> bars;  // strictly increasing dates
};

}  // namespace bt
