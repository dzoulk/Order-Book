// Differential fuzz test with a deliberately sparse price distribution:
// two price clusters far apart (around 100 and 999,000), chosen randomly
// per order, instead of fuzz_test.cpp's single narrow band. This is the
// shape of book that exposed the findNextOccupied pathology (see
// docs/HISTORY.md): best-price updates have to jump
// across a huge gap whenever one cluster's resting orders run out. A
// correctness bug in OccupancyBitmap's cross-cluster lookup would show up
// here even though it wouldn't necessarily show up in fuzz_test.cpp's
// narrow-band default.
//
// Deliberately small op count: this test's job is correctness coverage
// of cross-cluster transitions, not scale. Performance is sparse_bench's
// job (bench/sparse_bench_main.cpp).
//
// Configuration via environment variables:
//   ORDERBOOK_SPARSE_FUZZ_SEED  seed for reproduction (default: random)
//   ORDERBOOK_SPARSE_FUZZ_OPS   number of operations (default: 5000)

#include "orderbook/fast_order_book.hpp"
#include "orderbook/order_book.hpp"
#include "orderbook/reference_book.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <stdexcept>
#include <vector>

using namespace orderbook;

namespace {

constexpr Price kClusterA = 100;
constexpr Price kClusterB = 999'000;
constexpr Price kClusterHalfWidth = 5;

std::uint64_t seedFromEnvOrRandom() {
    if (const char* s = std::getenv("ORDERBOOK_SPARSE_FUZZ_SEED")) {
        return std::strtoull(s, nullptr, 10);
    }
    std::random_device rd;
    return (static_cast<std::uint64_t>(rd()) << 32) | rd();
}

std::size_t opCountFromEnvOrDefault() {
    if (const char* s = std::getenv("ORDERBOOK_SPARSE_FUZZ_OPS")) {
        return static_cast<std::size_t>(std::strtoull(s, nullptr, 10));
    }
    return 5'000;
}

template <typename Book>
bool statesMatch(const NaiveOrderBook& naive, const Book& other, const char* label) {
    if (naive.bestBid() != other.bestBid()) {
        std::fprintf(stderr, "bestBid mismatch (naive vs %s)\n", label);
        return false;
    }
    if (naive.bestAsk() != other.bestAsk()) {
        std::fprintf(stderr, "bestAsk mismatch (naive vs %s)\n", label);
        return false;
    }
    for (Price center : {kClusterA, kClusterB}) {
        for (Price px = center - kClusterHalfWidth; px <= center + kClusterHalfWidth; ++px) {
            for (Side side : {Side::Buy, Side::Sell}) {
                if (naive.depthAt(side, px) != other.depthAt(side, px)) {
                    std::fprintf(stderr, "depthAt mismatch at price %lld (naive vs %s)\n",
                                 static_cast<long long>(px), label);
                    return false;
                }
            }
        }
    }
    return true;
}

bool tradesMatch(const std::vector<Trade>& a, const std::vector<Trade>& b, const char* label) {
    if (a.size() != b.size()) {
        std::fprintf(stderr, "trade count mismatch (naive vs %s): naive=%zu other=%zu\n", label, a.size(),
                     b.size());
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (a[i].buyOrderId != b[i].buyOrderId || a[i].sellOrderId != b[i].sellOrderId ||
            a[i].price != b[i].price || a[i].qty != b[i].qty || a[i].seq != b[i].seq) {
            std::fprintf(stderr, "trade %zu mismatch (naive vs %s)\n", i, label);
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
                 "sparse fuzz: seed=%llu ops=%zu clusters=[%lld,%lld] "
                 "(set ORDERBOOK_SPARSE_FUZZ_SEED / ORDERBOOK_SPARSE_FUZZ_OPS to reproduce or scale)\n",
                 static_cast<unsigned long long>(seed), opCount, static_cast<long long>(kClusterA),
                 static_cast<long long>(kClusterB));

    std::mt19937_64 rng(seed);
    std::uniform_int_distribution<int> opPicker(1, 100);
    std::uniform_int_distribution<int> clusterPicker(0, 1);
    std::uniform_int_distribution<Price> offsetPicker(-kClusterHalfWidth, kClusterHalfWidth);
    std::uniform_int_distribution<Qty> qtyPicker(1, 20);
    std::uniform_int_distribution<int> sidePicker(0, 1);

    NaiveOrderBook naive;
    OrderBook real;
    FastOrderBook fast;

    OrderId nextId = 1;
    std::vector<OrderId> everAdded;

    for (std::size_t i = 0; i < opCount; ++i) {
        int pick = opPicker(rng);
        Side side = sidePicker(rng) == 0 ? Side::Buy : Side::Sell;
        Price center = clusterPicker(rng) == 0 ? kClusterA : kClusterB;

        if (pick <= 45 || everAdded.empty()) {
            OrderId id = nextId++;
            Price px = center + offsetPicker(rng);
            Qty qty = qtyPicker(rng);
            std::vector<Trade> naiveTrades = naive.addLimit(id, side, px, qty);
            std::vector<Trade> realTrades = real.addLimit(id, side, px, qty);
            std::vector<Trade> fastTrades = fast.addLimit(id, side, px, qty);
            everAdded.push_back(id);

            if (!tradesMatch(naiveTrades, realTrades, "v1") || !tradesMatch(naiveTrades, fastTrades, "v2")) {
                std::fprintf(stderr, "mismatch at op %zu (seed=%llu)\n", i,
                             static_cast<unsigned long long>(seed));
                return 1;
            }
        } else if (pick <= 60) {
            std::size_t windowStart = everAdded.size() > 50 ? everAdded.size() - 50 : 0;
            std::uniform_int_distribution<std::size_t> idxPicker(windowStart, everAdded.size() - 1);
            OrderId target = everAdded[idxPicker(rng)];

            bool naiveResult = naive.cancel(target);
            bool realResult = real.cancel(target);
            bool fastResult = fast.cancel(target);
            if (naiveResult != realResult || naiveResult != fastResult) {
                std::fprintf(stderr,
                             "cancel result mismatch at op %zu (seed=%llu): naive=%d v1=%d v2=%d\n", i,
                             static_cast<unsigned long long>(seed), naiveResult, realResult, fastResult);
                return 1;
            }
        } else if (pick <= 75) {
            std::size_t windowStart = everAdded.size() > 50 ? everAdded.size() - 50 : 0;
            std::uniform_int_distribution<std::size_t> idxPicker(windowStart, everAdded.size() - 1);
            OrderId target = everAdded[idxPicker(rng)];
            Qty newQty = qtyPicker(rng);

            bool naiveThrew = false, realThrew = false, fastThrew = false;
            bool naiveResult = false, realResult = false, fastResult = false;
            try {
                naiveResult = naive.reduceQty(target, newQty);
            } catch (const std::invalid_argument&) {
                naiveThrew = true;
            }
            try {
                realResult = real.reduceQty(target, newQty);
            } catch (const std::invalid_argument&) {
                realThrew = true;
            }
            try {
                fastResult = fast.reduceQty(target, newQty);
            } catch (const std::invalid_argument&) {
                fastThrew = true;
            }

            if (naiveThrew != realThrew || naiveThrew != fastThrew) {
                std::fprintf(stderr, "reduceQty throw mismatch at op %zu (seed=%llu)\n", i,
                             static_cast<unsigned long long>(seed));
                return 1;
            }
            if (!naiveThrew && (naiveResult != realResult || naiveResult != fastResult)) {
                std::fprintf(stderr, "reduceQty result mismatch at op %zu (seed=%llu)\n", i,
                             static_cast<unsigned long long>(seed));
                return 1;
            }
        } else if (pick <= 90) {
            // Reuses `center`, independently rerolled this iteration: a
            // meaningful fraction of these replaces jump the order from
            // whichever cluster it's at to the *other* cluster, exactly
            // the cross-cluster transition this file exists to stress.
            std::size_t windowStart = everAdded.size() > 50 ? everAdded.size() - 50 : 0;
            std::uniform_int_distribution<std::size_t> idxPicker(windowStart, everAdded.size() - 1);
            OrderId target = everAdded[idxPicker(rng)];
            Price newPx = center + offsetPicker(rng);

            bool naiveThrew = false, realThrew = false, fastThrew = false;
            std::vector<Trade> naiveTrades, realTrades, fastTrades;
            try {
                naiveTrades = naive.replacePrice(target, newPx);
            } catch (const std::invalid_argument&) {
                naiveThrew = true;
            }
            try {
                realTrades = real.replacePrice(target, newPx);
            } catch (const std::invalid_argument&) {
                realThrew = true;
            }
            try {
                fastTrades = fast.replacePrice(target, newPx);
            } catch (const std::invalid_argument&) {
                fastThrew = true;
            }

            if (naiveThrew != realThrew || naiveThrew != fastThrew) {
                std::fprintf(stderr, "replacePrice throw mismatch at op %zu (seed=%llu)\n", i,
                             static_cast<unsigned long long>(seed));
                return 1;
            }
            if (!naiveThrew && (!tradesMatch(naiveTrades, realTrades, "v1") ||
                                 !tradesMatch(naiveTrades, fastTrades, "v2"))) {
                std::fprintf(stderr, "mismatch at op %zu (seed=%llu)\n", i,
                             static_cast<unsigned long long>(seed));
                return 1;
            }
        } else {
            OrderId id = nextId++;
            Qty qty = qtyPicker(rng);
            std::vector<Trade> naiveTrades = naive.addMarket(id, side, qty);
            std::vector<Trade> realTrades = real.addMarket(id, side, qty);
            std::vector<Trade> fastTrades = fast.addMarket(id, side, qty);

            if (!tradesMatch(naiveTrades, realTrades, "v1") || !tradesMatch(naiveTrades, fastTrades, "v2")) {
                std::fprintf(stderr, "mismatch at op %zu (seed=%llu)\n", i,
                             static_cast<unsigned long long>(seed));
                return 1;
            }
        }

        if (!statesMatch(naive, real, "v1") || !statesMatch(naive, fast, "v2")) {
            std::fprintf(stderr, "mismatch at op %zu (seed=%llu)\n", i,
                         static_cast<unsigned long long>(seed));
            return 1;
        }
    }

    std::fprintf(stderr, "sparse fuzz: OK, %zu ops, seed=%llu\n", opCount, static_cast<unsigned long long>(seed));
    return 0;
}
