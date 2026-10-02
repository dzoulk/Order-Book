// Throughput/latency benchmark for the real OrderBook (milestone 4).
// NaiveOrderBook is never benchmarked here: it's O(n) by design and exists
// purely as a correctness oracle for the fuzz test, not a baseline to beat.
//
// The operation stream is fully pre-generated before timing starts, so RNG
// and distribution-sampling overhead never lands inside a measured latency
// sample. Only the OrderBook method calls themselves are timed.
//
// Configuration via environment variables:
//   BENCH_SEED         seed for reproduction (default: random)
//   BENCH_OPS          operations to measure (default: 1000000)
//   BENCH_WARMUP_OPS   operations to run unmeasured first (default: 50000)
//   BENCH_OUTPUT       path for raw per-op latencies, one ns value per line
//                      (default: bench/results/latencies_v1.csv)

#include "orderbook/order_book.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>

using namespace orderbook;
using Clock = std::chrono::steady_clock;

namespace {

std::uint64_t seedFromEnvOrRandom() {
    if (const char* s = std::getenv("BENCH_SEED")) return std::strtoull(s, nullptr, 10);
    std::random_device rd;
    return (static_cast<std::uint64_t>(rd()) << 32) | rd();
}

std::size_t sizeFromEnvOrDefault(const char* name, std::size_t fallback) {
    if (const char* s = std::getenv(name)) return static_cast<std::size_t>(std::strtoull(s, nullptr, 10));
    return fallback;
}

std::string outputPathFromEnvOrDefault() {
    if (const char* s = std::getenv("BENCH_OUTPUT")) return s;
    return "bench/results/latencies_v1.csv";
}

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
std::vector<GeneratedOp> generateOps(std::size_t count, std::uint64_t seed) {
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

void apply(OrderBook& book, const GeneratedOp& op) {
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

double percentile(const std::vector<double>& sortedNs, double p) {
    if (sortedNs.empty()) return 0.0;
    std::size_t idx = static_cast<std::size_t>(p / 100.0 * static_cast<double>(sortedNs.size() - 1));
    return sortedNs[idx];
}

} // namespace

int main() {
    std::uint64_t seed = seedFromEnvOrRandom();
    std::size_t opCount = sizeFromEnvOrDefault("BENCH_OPS", 1'000'000);
    std::size_t warmupCount = sizeFromEnvOrDefault("BENCH_WARMUP_OPS", 50'000);
    std::string outputPath = outputPathFromEnvOrDefault();

    std::fprintf(stderr, "orderbook bench: generating %zu ops (seed=%llu)...\n",
                 warmupCount + opCount, static_cast<unsigned long long>(seed));
    std::vector<GeneratedOp> ops = generateOps(warmupCount + opCount, seed);

    OrderBook book;

    std::fprintf(stderr, "orderbook bench: warming up (%zu ops)...\n", warmupCount);
    for (std::size_t i = 0; i < warmupCount; ++i) {
        apply(book, ops[i]);
    }

    std::fprintf(stderr, "orderbook bench: measuring (%zu ops)...\n", opCount);
    std::vector<double> latenciesNs;
    latenciesNs.reserve(opCount);

    Clock::time_point start = Clock::now();
    for (std::size_t i = warmupCount; i < warmupCount + opCount; ++i) {
        Clock::time_point opStart = Clock::now();
        apply(book, ops[i]);
        Clock::time_point opEnd = Clock::now();
        latenciesNs.push_back(std::chrono::duration<double, std::nano>(opEnd - opStart).count());
    }
    Clock::time_point end = Clock::now();

    double totalSeconds = std::chrono::duration<double>(end - start).count();
    double throughput = static_cast<double>(opCount) / totalSeconds;

    std::vector<double> sorted = latenciesNs;
    std::sort(sorted.begin(), sorted.end());

    std::printf("\norderbook_bench v1 results\n");
    std::printf("  seed:          %llu\n", static_cast<unsigned long long>(seed));
    std::printf("  ops measured:  %zu (warmup %zu)\n", opCount, warmupCount);
    std::printf("  wall time:     %.3f s\n", totalSeconds);
    std::printf("  throughput:    %.0f ops/sec\n", throughput);
    std::printf("  p50 latency:   %.0f ns\n", percentile(sorted, 50));
    std::printf("  p99 latency:   %.0f ns\n", percentile(sorted, 99));
    std::printf("  p99.9 latency: %.0f ns\n", percentile(sorted, 99.9));

    std::filesystem::path outPath(outputPath);
    if (outPath.has_parent_path()) std::filesystem::create_directories(outPath.parent_path());
    std::ofstream out(outPath);
    out << "latency_ns\n";
    for (double v : latenciesNs) out << v << "\n";
    std::fprintf(stderr, "orderbook bench: raw latencies written to %s\n", outputPath.c_str());

    return 0;
}
