#pragma once

// Synthetic order-flow generator shared between bench_main (full
// throughput/latency harness) and profile_main (minimal perf driver).

#include "orderbook/order_book.hpp"
#include "orderbook/reference_book.hpp"

#include <cstdint>
#include <random>
#include <vector>

namespace orderbook_bench {

using namespace orderbook;

enum class OpKind { Limit, Cancel, Market, ReduceQty, ReplacePrice };

struct GeneratedOp {
    OpKind kind;
    OrderId id;    // new id for Limit/Market, target id for the others
    Side side;     // meaningful for Limit/Market
    Price price;   // meaningful for Limit; reused as the new price for ReplacePrice
    Qty qty;       // meaningful for Limit/Market; reused as the new qty for ReduceQty
};

// Generates the full operation stream up front. Two modes:
//   includeModifyOps=true  (bench_main): 45% limit, 20% cancel, 15%
//     reduceQty, 15% replacePrice, 5% market, the realistic mix the
//     project plan and the fuzz tests use.
//   includeModifyOps=false (profile_main): 60% limit, 30% cancel, 10%
//     market only, kept separate rather than sharing one mix and
//     filtering, so profile_main's profile stays exactly as clean as it
//     was before reduceQty/replacePrice existed.
//
// Generation drives a real NaiveOrderBook alongside building the op
// list, specifically so reduceQty/replacePrice targets and reduceQty's
// newQty are verified against the order's *actual* current quantity
// before being emitted, not guessed. An earlier version picked targets
// from a recency-biased window without checking, which meant a
// meaningful fraction of them were already stale by replay time
// (something else had consumed the order first); replaying a
// std::invalid_argument throw on the hot path blew up p99.9 by roughly
// 30x (exception unwinding costs microseconds, not nanoseconds), not a
// property of the engine being benchmarked, just of a sloppy generator.
// Running the exact same ops through NaiveOrderBook during generation
// means every emitted op is guaranteed valid against the one
// implementation already certified correct by the differential fuzz
// tests, so the real engines being benchmarked never throw at all.
inline std::vector<GeneratedOp> generateOps(std::size_t count, std::uint64_t seed, bool includeModifyOps = true) {
    std::mt19937_64 rng(seed);
    std::uniform_int_distribution<int> opPicker(1, 100);
    std::uniform_int_distribution<int> sidePicker(0, 1);
    std::uniform_int_distribution<Qty> qtyPicker(1, 20);
    std::uniform_int_distribution<Price> offsetPicker(-10, 10);
    std::uniform_int_distribution<int> midDriftChance(1, 100);
    std::uniform_int_distribution<int> midDriftStep(-1, 1);

    std::vector<GeneratedOp> ops;
    ops.reserve(count);

    NaiveOrderBook shadow;
    OrderId nextId = 1;
    std::vector<OrderId> everAdded;
    Price mid = 10'000;

    // Picks a recently-added id that's confirmed still resting in
    // `shadow` right now, or nullopt if none of the recent window is.
    auto pickValidRecentTarget = [&]() -> std::optional<OrderId> {
        std::size_t windowStart = everAdded.size() > 50 ? everAdded.size() - 50 : 0;
        for (std::size_t tries = 0; tries < 10 && windowStart < everAdded.size(); ++tries) {
            std::uniform_int_distribution<std::size_t> idxPicker(windowStart, everAdded.size() - 1);
            OrderId candidate = everAdded[idxPicker(rng)];
            if (shadow.restingQty(candidate).has_value()) return candidate;
        }
        return std::nullopt;
    };

    auto emitLimit = [&](Side side) {
        OrderId id = nextId++;
        Price px = static_cast<Price>(mid + offsetPicker(rng));
        Qty qty = qtyPicker(rng);
        shadow.addLimit(id, side, px, qty);
        ops.push_back(GeneratedOp{OpKind::Limit, id, side, px, qty});
        everAdded.push_back(id);
    };

    for (std::size_t i = 0; i < count; ++i) {
        if (midDriftChance(rng) <= 5) mid += midDriftStep(rng);

        int pick = opPicker(rng);
        Side side = sidePicker(rng) == 0 ? Side::Buy : Side::Sell;
        int limitCutoff = includeModifyOps ? 45 : 60;
        int cancelCutoff = includeModifyOps ? 65 : 90;
        int reduceCutoff = includeModifyOps ? 80 : cancelCutoff;   // no-op range when disabled
        int replaceCutoff = includeModifyOps ? 95 : cancelCutoff;  // no-op range when disabled

        if (pick <= limitCutoff || everAdded.empty()) {
            emitLimit(side);
        } else if (pick <= cancelCutoff) {
            auto target = pickValidRecentTarget();
            if (!target) {
                emitLimit(side);
                continue;
            }
            shadow.cancel(*target);
            ops.push_back(GeneratedOp{OpKind::Cancel, *target, Side::Buy, 0, 0});
        } else if (pick <= reduceCutoff) {
            auto target = pickValidRecentTarget();
            Qty currentQty = target ? *shadow.restingQty(*target) : 0;
            if (!target || currentQty < 2) {
                // Nothing valid to reduce (qty 1 can't go lower and still
                // be > 0); fall back to a limit order instead.
                emitLimit(side);
                continue;
            }
            Qty newQty = currentQty / 2;  // always a real reduction, never 0
            shadow.reduceQty(*target, newQty);
            ops.push_back(GeneratedOp{OpKind::ReduceQty, *target, Side::Buy, 0, newQty});
        } else if (pick <= replaceCutoff) {
            auto target = pickValidRecentTarget();
            if (!target) {
                emitLimit(side);
                continue;
            }
            Price newPx = static_cast<Price>(mid + offsetPicker(rng));
            shadow.replacePrice(*target, newPx);
            ops.push_back(GeneratedOp{OpKind::ReplacePrice, *target, Side::Buy, newPx, 0});
        } else {
            OrderId id = nextId++;
            Qty qty = qtyPicker(rng);
            shadow.addMarket(id, side, qty);
            ops.push_back(GeneratedOp{OpKind::Market, id, side, 0, qty});
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
        case OpKind::ReduceQty:
            book.reduceQty(op.id, op.qty);
            break;
        case OpKind::ReplacePrice:
            book.replacePrice(op.id, op.price);
            break;
    }
}

} // namespace orderbook_bench
