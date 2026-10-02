#pragma once

#include "orderbook/types.hpp"

#include <cstdint>
#include <optional>
#include <unordered_map>
#include <vector>

namespace orderbook {

// v2: same public interface as OrderBook, built for speed instead of
// simplicity. See docs/DESIGN.md milestone 5 for the profiling that
// motivated this design and the full v1-vs-v2 numbers.
//
// - bidLevels_/askLevels_: flat std::vector<Level>, indexed directly by
//   price. Replaces std::map's red-black tree (O(log n), one node alloc
//   per price level) with O(1) array indexing and no per-level alloc.
// - Orders live in a preallocated object pool (pool_) with prev/next
//   indices threaded through it, both for each price level's FIFO queue
//   and for the pool's own free list. No per-order heap allocation once
//   the pool covers the live order count.
// - bestBid()/bestAsk() are O(1) cached reads, updated on insert; a scan
//   (bounded by a live-count check, never scanning a genuinely empty
//   book) finds the next occupied level when the cached best empties out.
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
    std::optional<Price> findNextOccupied(const std::vector<Level>& levels, Price from, bool searchUpward) const;

    std::vector<Trade> match(Order& incoming, bool isMarket);
    void rest(const Order& order);

    std::vector<Level> bidLevels_;
    std::vector<Level> askLevels_;
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
