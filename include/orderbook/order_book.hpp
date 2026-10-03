#pragma once

#include "orderbook/types.hpp"

#include <list>
#include <map>
#include <optional>
#include <unordered_map>
#include <vector>

namespace orderbook {

// Price-time priority limit order book.
//
// v1 design (see docs/DESIGN.md for rationale):
//   bids_: std::map<Price, std::list<Order>, std::greater<Price>>.
//          begin() is always the best (highest) bid.
//   asks_: std::map<Price, std::list<Order>>.
//          begin() is always the best (lowest) ask.
//   index_: std::unordered_map<OrderId, Location> for O(1) cancel. Location
//           carries the side/price (to find the right map and level) plus a
//           std::list<Order>::iterator (to erase in O(1) once there). We
//           use std::list specifically because erasing one element never
//           invalidates iterators to any other element. A std::vector's
//           iterators would be invalidated by erasing an unrelated order.
class OrderBook {
public:
    // Adds a limit order, matching immediately against the opposite side
    // where price allows, and resting any unfilled remainder. Throws
    // std::invalid_argument on qty == 0 or a duplicate (currently resting) id.
    //
    // Returns a reference to an internal buffer that's overwritten by the
    // next addLimit/addMarket call. This exists so the common case (zero
    // or one fill) doesn't allocate a fresh vector every call, profiling
    // found that allocation on the hot path; see docs/DESIGN.md. Copy
    // the result (e.g. `std::vector<Trade> t = book.addLimit(...)`,
    // which is what every caller in this codebase does) if you need it
    // to outlive the next call.
    const std::vector<Trade>& addLimit(OrderId id, Side side, Price px, Qty qty);

    // Adds a market order, matching immediately against the opposite side
    // until filled or the book is exhausted. Any unfilled remainder is
    // discarded (market orders never rest). Throws std::invalid_argument
    // on qty == 0 or a duplicate (currently resting) id. Same buffer-reuse
    // caveat as addLimit.
    const std::vector<Trade>& addMarket(OrderId id, Side side, Qty qty);

    // Cancels a resting order. Returns false if id is unknown or already
    // fully filled/cancelled.
    bool cancel(OrderId id);

    std::optional<Price> bestBid() const;
    std::optional<Price> bestAsk() const;

    // Total resting quantity at a given price level on the given side.
    Qty depthAt(Side side, Price px) const;

private:
    struct Location {
        Side side;
        Price price;
        std::list<Order>::iterator it;
    };

    // Matches `incoming` against the opposite side, mutating incoming.qty
    // down as fills happen. Does not insert any unfilled remainder of
    // `incoming`; callers decide whether it rests (limit) or is discarded
    // (market). When isMarket is true, price is ignored entirely. Returns
    // a reference to tradeBuffer_, cleared (not reallocated) at the start
    // of every call.
    const std::vector<Trade>& match(Order& incoming, bool isMarket);

    // Inserts `order` as a new resting order at the back of its price
    // level's FIFO queue, and records its location in index_.
    void rest(const Order& order);

    std::map<Price, std::list<Order>, std::greater<Price>> bids_;
    std::map<Price, std::list<Order>> asks_;
    std::unordered_map<OrderId, Location> index_;
    SeqNum nextSeq_ = 0;
    SeqNum nextTradeSeq_ = 0;
    std::vector<Trade> tradeBuffer_;
};

} // namespace orderbook
