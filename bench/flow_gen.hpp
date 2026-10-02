#pragma once

// Synthetic order-flow generator shared between bench_main (full
// throughput/latency harness) and profile_main (minimal perf driver).

#include "orderbook/order_book.hpp"

#include <cstdint>
#include <random>
#include <vector>

namespace orderbook_bench {

using namespace orderbook;

enum class OpKind { Limit, Cancel, Market };

struct GeneratedOp {
    OpKind kind;
    OrderId id;    // new id for Limit/Market, cancel target for Cancel
    Side side;     // meaningful for Limit/Market
    Price price;   // meaningful for Limit
    Qty qty;       // meaningful for Limit/Market
};

// Generates the full operation stream up front: 60% limit, 30% cancel,
// 10% market, prices clustered in a small band around a midpoint that
// slowly drifts, matching the synthetic order-flow mix from the project
// plan. Cancels are biased toward recently added orders, more likely
// still resting, and fall back to a limit order when nothing is resting
// yet, keeping the requested op count exact.
inline std::vector<GeneratedOp> generateOps(std::size_t count, std::uint64_t seed) {
    std::mt19937_64 rng(seed);
    std::uniform_int_distribution<int> opPicker(1, 100);
    std::uniform_int_distribution<int> sidePicker(0, 1);
    std::uniform_int_distribution<Qty> qtyPicker(1, 20);
    std::uniform_int_distribution<Price> offsetPicker(-10, 10);
    std::uniform_int_distribution<int> midDriftChance(1, 100);
    std::uniform_int_distribution<int> midDriftStep(-1, 1);

    std::vector<GeneratedOp> ops;
    ops.reserve(count);

    OrderId nextId = 1;
    std::vector<OrderId> everAdded;
    Price mid = 10'000;

    for (std::size_t i = 0; i < count; ++i) {
        if (midDriftChance(rng) <= 5) mid += midDriftStep(rng);

        int pick = opPicker(rng);
        Side side = sidePicker(rng) == 0 ? Side::Buy : Side::Sell;

        if (pick <= 60 || everAdded.empty()) {
            OrderId id = nextId++;
            GeneratedOp op{OpKind::Limit, id, side, static_cast<Price>(mid + offsetPicker(rng)),
                           qtyPicker(rng)};
            ops.push_back(op);
            everAdded.push_back(id);
        } else if (pick <= 90) {
            std::size_t windowStart = everAdded.size() > 50 ? everAdded.size() - 50 : 0;
            std::uniform_int_distribution<std::size_t> idxPicker(windowStart, everAdded.size() - 1);
            GeneratedOp op{OpKind::Cancel, everAdded[idxPicker(rng)], Side::Buy, 0, 0};
            ops.push_back(op);
        } else {
            OrderId id = nextId++;
            GeneratedOp op{OpKind::Market, id, side, 0, qtyPicker(rng)};
            ops.push_back(op);
        }
    }
    return ops;
}

template <typename Book>
void apply(Book& book, const GeneratedOp& op) {
    switch (op.kind) {
        case OpKind::Limit:
            book.addLimit(op.id, op.side, op.price, op.qty);
            break;
        case OpKind::Cancel:
            book.cancel(op.id);
            break;
        case OpKind::Market:
            book.addMarket(op.id, op.side, op.qty);
            break;
    }
}

} // namespace orderbook_bench
