#pragma once
#include <vector>

#include "bt/bar.hpp"

namespace bt {

// Back-adjusts a series using the per-bar factor f = adj_close / close:
//   open' = open*f, high' = high*f, low' = low*f, close' = adj_close, adj_close' = adj_close.
// Volume is left untouched (a split changes share counts but a dividend does not; the ratio alone
// cannot tell them apart). Returns a new vector; the input is not modified.
// After scaling, high/low are widened to cover open/close so floating-point rounding can never
// produce a bar that fails check_bar().
std::vector<Bar> back_adjust(const std::vector<Bar>& bars);

}  // namespace bt
