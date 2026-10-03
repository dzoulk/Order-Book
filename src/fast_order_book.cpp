#include "orderbook/fast_order_book.hpp"

#include <algorithm>
#include <stdexcept>

namespace orderbook {

FastOrderBook::FastOrderBook() : bidLevels_(kMaxPrice), askLevels_(kMaxPrice) {
    pool_.reserve(1 << 16);  // generous for realistic concurrent resting-order counts
}

FastOrderBook::PoolIndex FastOrderBook::allocateNode(const Order& order) {
    if (freeListHead_ != kInvalidIndex) {
        PoolIndex idx = freeListHead_;
        freeListHead_ = pool_[idx].next;
        pool_[idx] = PoolNode{order, kInvalidIndex, kInvalidIndex};
        return idx;
    }
    PoolIndex idx = static_cast<PoolIndex>(pool_.size());
    pool_.push_back(PoolNode{order, kInvalidIndex, kInvalidIndex});
    return idx;
}

void FastOrderBook::freeNode(PoolIndex idx) {
    pool_[idx].next = freeListHead_;
    pool_[idx].prev = kInvalidIndex;
    freeListHead_ = idx;
}

void FastOrderBook::pushBack(Level& level, PoolIndex idx) {
    pool_[idx].prev = level.tail;
    pool_[idx].next = kInvalidIndex;
    if (level.tail != kInvalidIndex) {
        pool_[level.tail].next = idx;
    } else {
        level.head = idx;
    }
    level.tail = idx;
}

void FastOrderBook::unlink(Level& level, PoolIndex idx) {
    PoolNode& node = pool_[idx];
    if (node.prev != kInvalidIndex) {
        pool_[node.prev].next = node.next;
    } else {
        level.head = node.next;
    }
    if (node.next != kInvalidIndex) {
        pool_[node.next].prev = node.prev;
    } else {
        level.tail = node.prev;
    }
}

std::optional<Price> FastOrderBook::findNextOccupied(const OccupancyBitmap& occupied, Price from,
                                                       bool searchUpward) const {
    if (searchUpward) {
        auto found = occupied.findFirstSetFrom(static_cast<std::size_t>(from + 1));
        return found ? std::optional<Price>(static_cast<Price>(*found)) : std::nullopt;
    }
    if (from <= 0) return std::nullopt;
    auto found = occupied.findLastSetUpTo(static_cast<std::size_t>(from - 1));
    return found ? std::optional<Price>(static_cast<Price>(*found)) : std::nullopt;
}

const std::vector<Trade>& FastOrderBook::match(Order& incoming, bool isMarket) {
    tradeBuffer_.clear();  // keeps capacity: no allocation once warmed up
    bool isBuy = incoming.side == Side::Buy;
    std::vector<Level>& oppositeLevels = isBuy ? askLevels_ : bidLevels_;
    OccupancyBitmap& oppositeOccupied = isBuy ? askOccupied_ : bidOccupied_;
    std::optional<Price>& oppositeBest = isBuy ? bestAskPrice_ : bestBidPrice_;
    std::size_t& oppositeCount = isBuy ? askCount_ : bidCount_;

    while (incoming.qty > 0 && oppositeBest.has_value()) {
        Price levelPrice = *oppositeBest;

        if (!isMarket) {
            bool crosses = isBuy ? incoming.price >= levelPrice : incoming.price <= levelPrice;
            if (!crosses) break;
        }

        Level& level = oppositeLevels[static_cast<std::size_t>(levelPrice)];
        PoolIndex counterpartyIdx = level.head;
        Order& counterparty = pool_[counterpartyIdx].order;

        Qty tradeQty = std::min(incoming.qty, counterparty.qty);
        Trade t;
        t.price = levelPrice;
        t.qty = tradeQty;
        t.seq = nextTradeSeq_++;
        if (isBuy) {
            t.buyOrderId = incoming.id;
            t.sellOrderId = counterparty.id;
        } else {
            t.buyOrderId = counterparty.id;
            t.sellOrderId = incoming.id;
        }
        tradeBuffer_.push_back(t);

        incoming.qty -= tradeQty;
        counterparty.qty -= tradeQty;

        if (counterparty.qty == 0) {
            OrderId counterpartyId = counterparty.id;
            unlink(level, counterpartyIdx);
            freeNode(counterpartyIdx);
            index_.erase(counterpartyId);
            --oppositeCount;

            if (level.head == kInvalidIndex) {
                oppositeOccupied.clear(static_cast<std::size_t>(levelPrice));
                oppositeBest = (oppositeCount == 0) ? std::nullopt
                                                     : findNextOccupied(oppositeOccupied, levelPrice, isBuy);
            }
        }
    }

    return tradeBuffer_;
}

void FastOrderBook::rest(const Order& order) {
    if (order.side == Side::Buy) {
        PoolIndex idx = allocateNode(order);
        pushBack(bidLevels_[static_cast<std::size_t>(order.price)], idx);
        bidOccupied_.set(static_cast<std::size_t>(order.price));
        index_[order.id] = idx;
        ++bidCount_;
        if (!bestBidPrice_.has_value() || order.price > *bestBidPrice_) bestBidPrice_ = order.price;
    } else {
        PoolIndex idx = allocateNode(order);
        pushBack(askLevels_[static_cast<std::size_t>(order.price)], idx);
        askOccupied_.set(static_cast<std::size_t>(order.price));
        index_[order.id] = idx;
        ++askCount_;
        if (!bestAskPrice_.has_value() || order.price < *bestAskPrice_) bestAskPrice_ = order.price;
    }
}

const std::vector<Trade>& FastOrderBook::addLimit(OrderId id, Side side, Price px, Qty qty) {
    if (qty == 0) throw std::invalid_argument("FastOrderBook::addLimit: qty must be > 0");
    if (px < 0 || px >= kMaxPrice) throw std::out_of_range("FastOrderBook::addLimit: price out of supported band");
    if (index_.contains(id)) throw std::invalid_argument("FastOrderBook::addLimit: duplicate id");

    Order incoming{id, side, px, qty, nextSeq_++};
    const std::vector<Trade>& trades = match(incoming, /*isMarket=*/false);

    if (incoming.qty > 0) {
        rest(incoming);
    }
    return trades;
}

const std::vector<Trade>& FastOrderBook::addMarket(OrderId id, Side side, Qty qty) {
    if (qty == 0) throw std::invalid_argument("FastOrderBook::addMarket: qty must be > 0");
    if (index_.contains(id)) throw std::invalid_argument("FastOrderBook::addMarket: duplicate id");

    Order incoming{id, side, /*price=*/0, qty, nextSeq_++};
    return match(incoming, /*isMarket=*/true);
}

bool FastOrderBook::cancel(OrderId id) {
    auto it = index_.find(id);
    if (it == index_.end()) return false;

    PoolIndex idx = it->second;
    Order order = pool_[idx].order;  // copy: order fields are needed after unlink/free below

    if (order.side == Side::Buy) {
        Level& level = bidLevels_[static_cast<std::size_t>(order.price)];
        unlink(level, idx);
        --bidCount_;
        if (level.head == kInvalidIndex) {
            bidOccupied_.clear(static_cast<std::size_t>(order.price));
            if (bestBidPrice_ == order.price) {
                bestBidPrice_ = (bidCount_ == 0)
                                    ? std::nullopt
                                    : findNextOccupied(bidOccupied_, order.price, /*searchUpward=*/false);
            }
        }
    } else {
        Level& level = askLevels_[static_cast<std::size_t>(order.price)];
        unlink(level, idx);
        --askCount_;
        if (level.head == kInvalidIndex) {
            askOccupied_.clear(static_cast<std::size_t>(order.price));
            if (bestAskPrice_ == order.price) {
                bestAskPrice_ = (askCount_ == 0)
                                    ? std::nullopt
                                    : findNextOccupied(askOccupied_, order.price, /*searchUpward=*/true);
            }
        }
    }

    freeNode(idx);
    index_.erase(it);
    return true;
}

std::optional<Price> FastOrderBook::bestBid() const {
    return bestBidPrice_;
}

std::optional<Price> FastOrderBook::bestAsk() const {
    return bestAskPrice_;
}

Qty FastOrderBook::depthAt(Side side, Price px) const {
    if (px < 0 || px >= kMaxPrice) return 0;

    const std::vector<Level>& levels = (side == Side::Buy) ? bidLevels_ : askLevels_;
    const Level& level = levels[static_cast<std::size_t>(px)];

    Qty total = 0;
    PoolIndex cur = level.head;
    while (cur != kInvalidIndex) {
        total += pool_[cur].order.qty;
        cur = pool_[cur].next;
    }
    return total;
}

} // namespace orderbook
