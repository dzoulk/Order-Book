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

    // Reduces a resting order's quantity in place, keeping its FIFO
    // priority (it's still the same order, just asking for less).
    // Returns false if id is unknown. Throws std::invalid_argument if
    // newQty is 0 or >= the order's current quantity: this is a
    // reduction, not a cancel (use cancel for qty 0) or an increase
    // (not supported without losing priority, see replacePrice).
    bool reduceQty(OrderId id, Qty newQty);

    // Cancels the resting order at its old price and re-inserts it at
    // newPrice with a fresh sequence number, losing FIFO priority (it's
    // a different price level, there's no "keeping its place" there).
    // Quantity carries over unchanged. May match immediately if newPrice
    // crosses the book. Throws std::invalid_argument if id is unknown.
    std::vector<Trade> replacePrice(OrderId id, Price newPrice);

    std::optional<Price> bestBid() const;
    std::optional<Price> bestAsk() const;
    Qty depthAt(Side side, Price px) const;

    // Current remaining quantity of a resting order, or nullopt if id is
    // unknown. Lets callers (e.g. the benchmark's flow generator, which
    // needs to know an order's true current quantity to avoid generating
    // an invalid reduceQty) query exact state without duplicating this
    // book's matching logic themselves.
    std::optional<Qty> restingQty(OrderId id) const;

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
