#include "orderbook/order_book.hpp"

#include <algorithm>
#include <stdexcept>

namespace orderbook {

const std::vector<Trade>& OrderBook::match(Order& incoming, bool isMarket) {
    tradeBuffer_.clear();  // keeps capacity: no allocation once warmed up

    // Generic over bids_/asks_: they're different types (different Compare
    // template argument on std::map), so a single loop body needs to be
    // templated on the map type rather than bound to a reference. A
    // generic lambda handles that without duplicating the loop per side.
    auto matchLoop = [&](auto& opposite) {
        while (incoming.qty > 0 && !opposite.empty()) {
            auto levelIt = opposite.begin();  // best opposite price level
            Price levelPrice = levelIt->first;

            if (!isMarket) {
                bool crosses = incoming.side == Side::Buy
                                   ? incoming.price >= levelPrice
                                   : incoming.price <= levelPrice;
                if (!crosses) break;
            }

            std::list<Order>& level = levelIt->second;
            Order& counterparty = level.front();  // oldest order at this price
            Qty tradeQty = std::min(incoming.qty, counterparty.qty);

            Trade t;
            t.price = levelPrice;  // resting order sets the execution price
            t.qty = tradeQty;
            t.seq = nextTradeSeq_++;
            if (incoming.side == Side::Buy) {
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
                index_.erase(counterparty.id);
                level.pop_front();
                if (level.empty()) opposite.erase(levelIt);
            }
        }
    };

    if (incoming.side == Side::Buy) {
        matchLoop(asks_);
    } else {
        matchLoop(bids_);
    }

    return tradeBuffer_;
}

void OrderBook::rest(const Order& order) {
    if (order.side == Side::Buy) {
        std::list<Order>& level = bids_[order.price];
        level.push_back(order);
        index_[order.id] = Location{order.side, order.price, std::prev(level.end())};
    } else {
        std::list<Order>& level = asks_[order.price];
        level.push_back(order);
        index_[order.id] = Location{order.side, order.price, std::prev(level.end())};
    }
}

const std::vector<Trade>& OrderBook::insertAndMatch(OrderId id, Side side, Price px, Qty qty) {
    Order incoming{id, side, px, qty, nextSeq_++};
    const std::vector<Trade>& trades = match(incoming, /*isMarket=*/false);

    if (incoming.qty > 0) {
        rest(incoming);
    }
    return trades;
}

const std::vector<Trade>& OrderBook::addLimit(OrderId id, Side side, Price px, Qty qty) {
    if (qty == 0) throw std::invalid_argument("OrderBook::addLimit: qty must be > 0");
    if (index_.contains(id)) throw std::invalid_argument("OrderBook::addLimit: duplicate id");
    return insertAndMatch(id, side, px, qty);
}

const std::vector<Trade>& OrderBook::addMarket(OrderId id, Side side, Qty qty) {
    if (qty == 0) throw std::invalid_argument("OrderBook::addMarket: qty must be > 0");
    if (index_.contains(id)) throw std::invalid_argument("OrderBook::addMarket: duplicate id");

    Order incoming{id, side, /*price=*/0, qty, nextSeq_++};
    return match(incoming, /*isMarket=*/true);
    // Any unfilled remainder is discarded: market orders never rest.
}

void OrderBook::removeFromBook(const Location& loc) {
    if (loc.side == Side::Buy) {
        auto levelIt = bids_.find(loc.price);
        levelIt->second.erase(loc.it);
        if (levelIt->second.empty()) bids_.erase(levelIt);
    } else {
        auto levelIt = asks_.find(loc.price);
        levelIt->second.erase(loc.it);
        if (levelIt->second.empty()) asks_.erase(levelIt);
    }
}

bool OrderBook::cancel(OrderId id) {
    auto it = index_.find(id);
    if (it == index_.end()) return false;

    removeFromBook(it->second);
    index_.erase(it);
    return true;
}

bool OrderBook::reduceQty(OrderId id, Qty newQty) {
    auto it = index_.find(id);
    if (it == index_.end()) return false;

    Qty currentQty = it->second.it->qty;
    if (newQty == 0 || newQty >= currentQty) {
        throw std::invalid_argument("OrderBook::reduceQty: newQty must be > 0 and < current qty");
    }
    it->second.it->qty = newQty;  // std::list iterator stability makes this a safe in-place mutation
    return true;
}

const std::vector<Trade>& OrderBook::replacePrice(OrderId id, Price newPrice) {
    auto it = index_.find(id);
    if (it == index_.end()) throw std::invalid_argument("OrderBook::replacePrice: unknown id");

    Location loc = it->second;
    Order order = *loc.it;  // copy: needed after removeFromBook invalidates loc.it below
    removeFromBook(loc);
    index_.erase(it);

    return insertAndMatch(id, order.side, newPrice, order.qty);
}

std::optional<Price> OrderBook::bestBid() const {
    if (bids_.empty()) return std::nullopt;
    return bids_.begin()->first;
}

std::optional<Price> OrderBook::bestAsk() const {
    if (asks_.empty()) return std::nullopt;
    return asks_.begin()->first;
}

Qty OrderBook::depthAt(Side side, Price px) const {
    Qty total = 0;
    if (side == Side::Buy) {
        auto it = bids_.find(px);
        if (it == bids_.end()) return 0;
        for (const Order& o : it->second) total += o.qty;
    } else {
        auto it = asks_.find(px);
        if (it == asks_.end()) return 0;
        for (const Order& o : it->second) total += o.qty;
    }
    return total;
}

} // namespace orderbook
