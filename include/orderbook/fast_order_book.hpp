#pragma once

#include "orderbook/occupancy_bitmap.hpp"
#include "orderbook/types.hpp"

#include <cstdint>
#include <optional>
#include <unordered_map>
#include <vector>

namespace orderbook {

// v2: same public interface as OrderBook, built for speed instead of
// simplicity. See docs/DESIGN.md milestone 5 for the profiling that
// motivated this design and the full v1-vs-v2 numbers, and the milestone
// 5 follow-up for why bestBid/bestAsk use a hierarchical bitmap rather
// than a plain linear scan.
//
// - bidLevels_/askLevels_: flat std::vector<Level>, indexed directly by
//   price. Replaces std::map's red-black tree (O(log n), one node alloc
//   per price level) with O(1) array indexing and no per-level alloc.
// - Orders live in a preallocated object pool (pool_) with prev/next
//   indices threaded through it, both for each price level's FIFO queue
//   and for the pool's own free list. No per-order heap allocation once
//   the pool covers the live order count.
// - bestBid()/bestAsk() are O(1) cached reads. When the cached best
//   level empties out, bidOccupied_/askOccupied_ (OccupancyBitmap) find
//   the next occupied price in O(1) (a handful of word operations,
//   independent of price range). An earlier version used a linear scan
//   over bidLevels_/askLevels_ here, which was genuinely O(price range)
//   in the worst case: a sparse book (one resting order far from where
//   activity is happening) made cancel/match thousands of times slower
//   than v1. That bug, how it was found, and the fix are in
//   docs/DESIGN.md.
//
// Known limitation: only prices in [0, kMaxPrice) are supported; addLimit
// throws std::out_of_range outside that band. OrderBook and
// NaiveOrderBook have no such restriction. See docs/DESIGN.md for why
// this trade is acceptable here.
class FastOrderBook {
public:
    static constexpr Price kMaxPrice = 1'000'000;

    FastOrderBook();

    std::vector<Trade> addLimit(OrderId id, Side side, Price px, Qty qty);
    std::vector<Trade> addMarket(OrderId id, Side side, Qty qty);
    bool cancel(OrderId id);
    std::optional<Price> bestBid() const;
    std::optional<Price> bestAsk() const;
    Qty depthAt(Side side, Price px) const;

private:
    using PoolIndex = std::uint32_t;
    static constexpr PoolIndex kInvalidIndex = static_cast<PoolIndex>(-1);

    struct Level {
        PoolIndex head = kInvalidIndex;
        PoolIndex tail = kInvalidIndex;
    };

    struct PoolNode {
        Order order{};
        PoolIndex prev = kInvalidIndex;
        PoolIndex next = kInvalidIndex;
    };

    PoolIndex allocateNode(const Order& order);
    void freeNode(PoolIndex idx);
    void pushBack(Level& level, PoolIndex idx);
    // Removes idx from `level`'s list. Does not update bestBid_/bestAsk_;
    // callers that empty out the current best level are responsible for
    // refreshing the cache via findNextOccupied.
    void unlink(Level& level, PoolIndex idx);
    // Next occupied price after `from` (searchUpward) or before it
    // (!searchUpward), via the matching occupancy bitmap. O(1): a few
    // word operations, independent of how far away the next level is.
    std::optional<Price> findNextOccupied(const OccupancyBitmap& occupied, Price from, bool searchUpward) const;

    std::vector<Trade> match(Order& incoming, bool isMarket);
    void rest(const Order& order);

    std::vector<Level> bidLevels_;
    std::vector<Level> askLevels_;
    OccupancyBitmap bidOccupied_{static_cast<std::size_t>(kMaxPrice)};
    OccupancyBitmap askOccupied_{static_cast<std::size_t>(kMaxPrice)};
    std::optional<Price> bestBidPrice_;
    std::optional<Price> bestAskPrice_;
    std::size_t bidCount_ = 0;  // live resting order count, each side
    std::size_t askCount_ = 0;

    std::vector<PoolNode> pool_;
    PoolIndex freeListHead_ = kInvalidIndex;

    std::unordered_map<OrderId, PoolIndex> index_;
    SeqNum nextSeq_ = 0;
    SeqNum nextTradeSeq_ = 0;
};

} // namespace orderbook
