#pragma once

#include <cstdint>

namespace orderbook {

using OrderId = std::uint64_t;
using Price   = std::int64_t;   // integer ticks (e.g. cents), never floating point
using Qty     = std::uint64_t;
using SeqNum  = std::uint64_t;  // assigned by the book; breaks price ties by arrival order

enum class Side { Buy, Sell };

struct Order {
    OrderId id;
    Side    side;
    Price   price;  // meaningless for market orders
    Qty     qty;    // remaining (unfilled) quantity
    SeqNum  seq;    // assigned on entry; lower seq = earlier arrival
};

struct Trade {
    OrderId buyOrderId;
    OrderId sellOrderId;
    Price   price;
    Qty     qty;
    SeqNum  seq;  // trade sequence number
};

} // namespace orderbook
