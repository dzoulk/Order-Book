// Differential fuzz test: feeds the same random stream of operations to
// NaiveOrderBook (obviously correct, linear scans) and OrderBook (the real
// v1 engine) and checks they agree after every single operation, both on
// the trades returned and on full book state. Prices are generated inside
// a fixed band around a midpoint, so a linear sweep of that band after
// every op is a complete check of book equality, not just bestBid/bestAsk.
// It's not a cheap check though: NaiveOrderBook's depthAt is O(n), called
// twice per price in the band, every single op. That cost is the price of
// checking full state after every op instead of just at the end, which is
// what makes a failure point straight at the operation that broke things
// instead of somewhere earlier in a long run.
//
// Standalone executable rather than a GoogleTest case since it's one long
// property check, not a suite of named cases. CTest just runs it and
// checks the exit code.
//
// Configuration via environment variables:
//   ORDERBOOK_FUZZ_SEED  seed for reproduction (default: random)
//   ORDERBOOK_FUZZ_OPS   number of operations to run (default: 20000, fast
//                        enough for routine local runs; set this much
//                        higher, e.g. into the millions, for a serious
//                        pre-milestone check)

#include "orderbook/order_book.hpp"
#include "orderbook/reference_book.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <vector>

using namespace orderbook;

namespace {

constexpr Price kMidPrice = 10'000;
constexpr Price kBand = 20;

std::uint64_t seedFromEnvOrRandom() {
    if (const char* s = std::getenv("ORDERBOOK_FUZZ_SEED")) {
        return std::strtoull(s, nullptr, 10);
    }
    std::random_device rd;
    return (static_cast<std::uint64_t>(rd()) << 32) | rd();
}

std::size_t opCountFromEnvOrDefault() {
    if (const char* s = std::getenv("ORDERBOOK_FUZZ_OPS")) {
        return static_cast<std::size_t>(std::strtoull(s, nullptr, 10));
    }
    return 20'000;
}

bool statesMatch(const NaiveOrderBook& naive, const OrderBook& real) {
    if (naive.bestBid() != real.bestBid()) {
        std::fprintf(stderr, "bestBid mismatch\n");
        return false;
    }
    if (naive.bestAsk() != real.bestAsk()) {
        std::fprintf(stderr, "bestAsk mismatch\n");
        return false;
    }
    for (Price px = kMidPrice - kBand; px <= kMidPrice + kBand; ++px) {
        for (Side side : {Side::Buy, Side::Sell}) {
            if (naive.depthAt(side, px) != real.depthAt(side, px)) {
                std::fprintf(stderr, "depthAt mismatch at price %lld\n", static_cast<long long>(px));
                return false;
            }
        }
    }
    return true;
}

bool tradesMatch(const std::vector<Trade>& a, const std::vector<Trade>& b) {
    if (a.size() != b.size()) {
        std::fprintf(stderr, "trade count mismatch: naive=%zu real=%zu\n", a.size(), b.size());
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (a[i].buyOrderId != b[i].buyOrderId || a[i].sellOrderId != b[i].sellOrderId ||
            a[i].price != b[i].price || a[i].qty != b[i].qty || a[i].seq != b[i].seq) {
            std::fprintf(stderr, "trade %zu mismatch\n", i);
            return false;
        }
    }
    return true;
}

} // namespace

int main() {
    std::uint64_t seed = seedFromEnvOrRandom();
    std::size_t opCount = opCountFromEnvOrDefault();
    std::fprintf(stderr,
                 "orderbook fuzz: seed=%llu ops=%zu "
                 "(set ORDERBOOK_FUZZ_SEED / ORDERBOOK_FUZZ_OPS to reproduce or scale)\n",
                 static_cast<unsigned long long>(seed), opCount);

    std::mt19937_64 rng(seed);
    std::uniform_int_distribution<int> opPicker(1, 100);  // 1-60 limit, 61-90 cancel, 91-100 market
    std::uniform_int_distribution<Price> pricePicker(kMidPrice - kBand, kMidPrice + kBand);
    std::uniform_int_distribution<Qty> qtyPicker(1, 20);
    std::uniform_int_distribution<int> sidePicker(0, 1);

    NaiveOrderBook naive;
    OrderBook real;

    OrderId nextId = 1;
    std::vector<OrderId> everAdded;  // cancel targets; only addLimit orders can be resting

    for (std::size_t i = 0; i < opCount; ++i) {
        int pick = opPicker(rng);
        Side side = sidePicker(rng) == 0 ? Side::Buy : Side::Sell;

        if (pick <= 60) {
            OrderId id = nextId++;
            Price px = pricePicker(rng);
            Qty qty = qtyPicker(rng);
            std::vector<Trade> naiveTrades = naive.addLimit(id, side, px, qty);
            std::vector<Trade> realTrades = real.addLimit(id, side, px, qty);
            everAdded.push_back(id);

            if (!tradesMatch(naiveTrades, realTrades)) {
                std::fprintf(stderr, "mismatch at op %zu (seed=%llu)\n", i,
                             static_cast<unsigned long long>(seed));
                return 1;
            }
        } else if (pick <= 90) {
            if (everAdded.empty()) continue;
            // Bias toward recently added orders: more likely still resting,
            // which exercises the true-result path more often than a
            // uniform pick over every id ever issued would.
            std::size_t windowStart = everAdded.size() > 50 ? everAdded.size() - 50 : 0;
            std::uniform_int_distribution<std::size_t> idxPicker(windowStart, everAdded.size() - 1);
            OrderId target = everAdded[idxPicker(rng)];

            bool naiveResult = naive.cancel(target);
            bool realResult = real.cancel(target);
            if (naiveResult != realResult) {
                std::fprintf(stderr, "cancel result mismatch at op %zu (seed=%llu): naive=%d real=%d\n",
                             i, static_cast<unsigned long long>(seed), naiveResult, realResult);
                return 1;
            }
        } else {
            OrderId id = nextId++;
            Qty qty = qtyPicker(rng);
            std::vector<Trade> naiveTrades = naive.addMarket(id, side, qty);
            std::vector<Trade> realTrades = real.addMarket(id, side, qty);

            if (!tradesMatch(naiveTrades, realTrades)) {
                std::fprintf(stderr, "mismatch at op %zu (seed=%llu)\n", i,
                             static_cast<unsigned long long>(seed));
                return 1;
            }
        }

        if (!statesMatch(naive, real)) {
            std::fprintf(stderr, "mismatch at op %zu (seed=%llu)\n", i,
                         static_cast<unsigned long long>(seed));
            return 1;
        }
    }

    std::fprintf(stderr, "orderbook fuzz: OK, %zu ops, seed=%llu\n", opCount,
                 static_cast<unsigned long long>(seed));
    return 0;
}
