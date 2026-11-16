#pragma once

namespace bt {

enum class Side { Buy, Sell };

inline const char* to_string(Side s) { return s == Side::Buy ? "Buy" : "Sell"; }

}  // namespace bt
