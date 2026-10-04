// Differential fuzz test: feeds the same random stream of operations to
// NaiveOrderBook (obviously correct, linear scans), OrderBook (v1), and
// FastOrderBook (v2), and checks the latter two agree with the naive book
// after every single operation, both on the trades returned and on full
// book state. Prices are generated inside a fixed band around a midpoint,
// so a linear sweep of that band after every op is a complete check of
// book equality, not just bestBid/bestAsk.
// It's not a cheap check though: NaiveOrderBook's depthAt is O(n), called
// twice per price in the band, every single op, for each of the two
// comparisons. That cost is the price of checking full state after every
// op instead of just at the end, which is what makes a failure point
// straight at the operation that broke things instead of somewhere
// earlier in a long run.
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
    for (Price px = kMidPrice - kBand; px <= kMidPrice + kBand; ++px) {
        for (Side side : {Side::Buy, Side::Sell}) {
            if (naive.depthAt(side, px) != other.depthAt(side, px)) {
                std::fprintf(stderr, "depthAt mismatch at price %lld (naive vs %s)\n",
                             static_cast<long long>(px), label);
                return false;
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
                 "orderbook fuzz: seed=%llu ops=%zu "
                 "(set ORDERBOOK_FUZZ_SEED / ORDERBOOK_FUZZ_OPS to reproduce or scale)\n",
                 static_cast<unsigned long long>(seed), opCount);

    std::mt19937_64 rng(seed);
    // 1-45 limit, 46-65 cancel, 66-80 reduceQty, 81-95 replacePrice, 96-100 market
    std::uniform_int_distribution<int> opPicker(1, 100);
    std::uniform_int_distribution<Price> pricePicker(kMidPrice - kBand, kMidPrice + kBand);
    std::uniform_int_distribution<Qty> qtyPicker(1, 20);
    std::uniform_int_distribution<int> sidePicker(0, 1);

    NaiveOrderBook naive;
    OrderBook real;
    FastOrderBook fast;

    OrderId nextId = 1;
    std::vector<OrderId> everAdded;  // cancel targets; only addLimit orders can be resting

    for (std::size_t i = 0; i < opCount; ++i) {
        int pick = opPicker(rng);
        Side side = sidePicker(rng) == 0 ? Side::Buy : Side::Sell;

        if (pick <= 45) {
            OrderId id = nextId++;
            Price px = pricePicker(rng);
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
        } else if (pick <= 65) {
            if (everAdded.empty()) continue;
            // Bias toward recently added orders: more likely still resting,
            // which exercises the true-result path more often than a
            // uniform pick over every id ever issued would.
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
        } else if (pick <= 80) {
            if (everAdded.empty()) continue;
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
        } else if (pick <= 95) {
            if (everAdded.empty()) continue;
            std::size_t windowStart = everAdded.size() > 50 ? everAdded.size() - 50 : 0;
            std::uniform_int_distribution<std::size_t> idxPicker(windowStart, everAdded.size() - 1);
            OrderId target = everAdded[idxPicker(rng)];
            Price newPx = pricePicker(rng);

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

    std::fprintf(stderr, "orderbook fuzz: OK, %zu ops, seed=%llu\n", opCount,
                 static_cast<unsigned long long>(seed));
    return 0;
}
