#include "bt/bar.hpp"

#include <cmath>

namespace bt {

bool operator==(const Bar& a, const Bar& b) {
  return a.date == b.date && a.open == b.open && a.high == b.high && a.low == b.low &&
         a.close == b.close && a.adj_close == b.adj_close && a.volume == b.volume;
}

const char* to_string(DataError e) {
  switch (e) {
    case DataError::FileOpen: return "FileOpen";
    case DataError::BadHeader: return "BadHeader";
    case DataError::BadFieldCount: return "BadFieldCount";
    case DataError::BadDate: return "BadDate";
    case DataError::BadNumber: return "BadNumber";
    case DataError::NullField: return "NullField";
    case DataError::NonPositivePrice: return "NonPositivePrice";
    case DataError::NegativeVolume: return "NegativeVolume";
    case DataError::HighBelowLow: return "HighBelowLow";
    case DataError::OpenOutsideRange: return "OpenOutsideRange";
    case DataError::CloseOutsideRange: return "CloseOutsideRange";
    case DataError::DuplicateDate: return "DuplicateDate";
    case DataError::NonIncreasingDate: return "NonIncreasingDate";
    case DataError::NoData: return "NoData";
  }
  return "Unknown";
}

std::optional<DataError> check_bar(const Bar& b) {
  const double px[5] = {b.open, b.high, b.low, b.close, b.adj_close};
  for (double p : px)
    if (!std::isfinite(p)) return DataError::BadNumber;
  for (double p : px)
    if (p <= 0.0) return DataError::NonPositivePrice;
  if (b.high < b.low) return DataError::HighBelowLow;
  if (b.open < b.low || b.open > b.high) return DataError::OpenOutsideRange;
  if (b.close < b.low || b.close > b.high) return DataError::CloseOutsideRange;
  if (b.volume < 0) return DataError::NegativeVolume;
  return std::nullopt;
}

}  // namespace bt
