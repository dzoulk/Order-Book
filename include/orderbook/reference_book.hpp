#pragma once

#include "orderbook/types.hpp"

#include <optional>
#include <vector>

namespace orderbook {

// Deliberately naive reference implementation: linear scans over a flat
// vector, no fancy data structures. Used to differential-fuzz the real
// OrderBook against (see tests/, milestone 3). Never optimize this file;
// its entire value is being obviously correct.
class NaiveOrderBook {
public:
    std::vector<Trade> addLimit(OrderId id, Side side, Price px, Qty qty);
    std::vector<Trade> addMarket(OrderId id, Side side, Qty qty);
    bool cancel(OrderId id);
    std::optional<Price> bestBid() const;
    std::optional<Price> bestAsk() const;
    Qty depthAt(Side side, Price px) const;

private:
    // Matches `incoming` against resting_ in place, mutating incoming.qty
    // down as fills happen. Does not insert any unfilled remainder of
    // `incoming`; callers decide whether it rests (limit) or is discarded
    // (market). When isMarket is true, price is ignored entirely.
    std::vector<Trade> match(Order& incoming, bool isMarket);

    std::vector<Order> resting_;
    SeqNum nextSeq_ = 0;
    SeqNum nextTradeSeq_ = 0;
};

} // namespace orderbook
