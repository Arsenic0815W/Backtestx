#pragma once
#include <cstddef>
#include <cstdint>

#include "bt/date.hpp"
#include "bt/money.hpp"
#include "bt/types.hpp"

namespace bt {

using OrderId = std::int64_t;  // 1, 2, 3 ... per engine run, in submission order (deterministic)

enum class OrderType { Market, Limit, Stop };

// Day: live for exactly ONE bar - the first loop iteration after submission - then expires, whether or not
//      the symbol traded that day and including any unfilled remainder of a partial fill.
// GTC: stays pending until filled, rejected or cancelled.
enum class TimeInForce { Day, GTC };

struct Order {
  OrderId id = 0;
  std::size_t symbol = 0;  // index into the AlignedData symbol list
  Side side = Side::Buy;
  OrderType type = OrderType::Market;
  std::int64_t qty = 0;        // as submitted
  std::int64_t remaining = 0;  // still unfilled
  Paise price = 0;             // limit or stop level, in paise; 0 for Market
  TimeInForce tif = TimeInForce::Day;
  Date submitted = 0;
};

// Why the broker simulator refused to execute an order at fill time.
enum class RejectReason {
  InsufficientCash,  // cash after the fill (including all fees) would be negative
  ShortingDisabled,  // a sell would take the position below zero while allow_short is false
  CrossesZero,       // one fill may not flip long<->short; close first, then open in a second order
};

const char* to_string(RejectReason r);

enum class OrderEventKind { Rejected, Expired, Cancelled };

struct OrderEvent {
  OrderId id = 0;
  Date date = 0;
  std::size_t symbol = 0;
  OrderEventKind kind = OrderEventKind::Rejected;
  RejectReason reason = RejectReason::InsufficientCash;  // meaningful only when kind == Rejected
};

}  // namespace bt
