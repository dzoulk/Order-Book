#include "orderbook/reference_book.hpp"

#include <algorithm>
#include <cstddef>
#include <stdexcept>

namespace orderbook {

namespace {

bool priceCrosses(Side incomingSide, Price incomingPx, Price restingPx) {
    return incomingSide == Side::Buy ? incomingPx >= restingPx
                                      : incomingPx <= restingPx;
}

} // namespace

std::vector<Trade> NaiveOrderBook::match(Order& incoming, bool isMarket) {
    std::vector<Trade> trades;

    for (;;) {
        if (incoming.qty == 0) break;

        // Find the best-priced opposite-side resting order the incoming
        // order is willing to trade with. It's a linear scan because this
        // file is meant to be obviously correct, not fast.
        std::size_t bestIdx = resting_.size();
        for (std::size_t i = 0; i < resting_.size(); ++i) {
            const Order& r = resting_[i];
            if (r.side == incoming.side) continue;
            if (!isMarket && !priceCrosses(incoming.side, incoming.price, r.price)) continue;

            if (bestIdx == resting_.size()) {
                bestIdx = i;
                continue;
            }
            const Order& best = resting_[bestIdx];
            bool better;
            if (incoming.side == Side::Buy) {
                // incoming buys; prefer the lowest resting ask, then oldest.
                better = (r.price < best.price) || (r.price == best.price && r.seq < best.seq);
            } else {
                // incoming sells; prefer the highest resting bid, then oldest.
                better = (r.price > best.price) || (r.price == best.price && r.seq < best.seq);
            }
            if (better) bestIdx = i;
        }

        if (bestIdx == resting_.size()) break;  // no eligible counterparty

        Order& counterparty = resting_[bestIdx];
        Qty tradeQty = std::min(incoming.qty, counterparty.qty);

        Trade t;
        t.price = counterparty.price;  // resting order sets the execution price
        t.qty = tradeQty;
        t.seq = nextTradeSeq_++;
        if (incoming.side == Side::Buy) {
            t.buyOrderId = incoming.id;
            t.sellOrderId = counterparty.id;
        } else {
            t.buyOrderId = counterparty.id;
            t.sellOrderId = incoming.id;
        }
        trades.push_back(t);

        incoming.qty -= tradeQty;
        counterparty.qty -= tradeQty;

        if (counterparty.qty == 0) {
            resting_[bestIdx] = resting_.back();
            resting_.pop_back();
        }
    }

    return trades;
}

std::vector<Trade> NaiveOrderBook::addLimit(OrderId id, Side side, Price px, Qty qty) {
    if (qty == 0) throw std::invalid_argument("NaiveOrderBook::addLimit: qty must be > 0");
    for (const Order& o : resting_) {
        if (o.id == id) throw std::invalid_argument("NaiveOrderBook::addLimit: duplicate id");
    }

    Order incoming{id, side, px, qty, nextSeq_++};
    std::vector<Trade> trades = match(incoming, /*isMarket=*/false);

    if (incoming.qty > 0) {
        resting_.push_back(incoming);
    }
    return trades;
}

std::vector<Trade> NaiveOrderBook::addMarket(OrderId id, Side side, Qty qty) {
    if (qty == 0) throw std::invalid_argument("NaiveOrderBook::addMarket: qty must be > 0");
    for (const Order& o : resting_) {
        if (o.id == id) throw std::invalid_argument("NaiveOrderBook::addMarket: duplicate id");
    }

    Order incoming{id, side, /*price=*/0, qty, nextSeq_++};
    return match(incoming, /*isMarket=*/true);
    // Any unfilled remainder is discarded: market orders never rest.
}

bool NaiveOrderBook::cancel(OrderId id) {
    for (std::size_t i = 0; i < resting_.size(); ++i) {
        if (resting_[i].id == id) {
            resting_[i] = resting_.back();
            resting_.pop_back();
            return true;
        }
    }
    return false;
}

std::optional<Price> NaiveOrderBook::bestBid() const {
    std::optional<Price> best;
    for (const Order& o : resting_) {
        if (o.side != Side::Buy) continue;
        if (!best || o.price > *best) best = o.price;
    }
    return best;
}

std::optional<Price> NaiveOrderBook::bestAsk() const {
    std::optional<Price> best;
    for (const Order& o : resting_) {
        if (o.side != Side::Sell) continue;
        if (!best || o.price < *best) best = o.price;
    }
    return best;
}

Qty NaiveOrderBook::depthAt(Side side, Price px) const {
    Qty total = 0;
    for (const Order& o : resting_) {
        if (o.side == side && o.price == px) total += o.qty;
    }
    return total;
}

} // namespace orderbook
