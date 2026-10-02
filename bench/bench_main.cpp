// Throughput/latency benchmark, v1 (OrderBook) and v2 (FastOrderBook) in
// one run so the numbers are directly comparable. NaiveOrderBook is never
// benchmarked here: it's O(n) by design and exists purely as a
// correctness oracle for the fuzz test, not a baseline to beat.
//
// The operation stream is fully pre-generated before timing starts, so RNG
// and distribution-sampling overhead never lands inside a measured latency
// sample. Only the book's own method call is timed. Both engines run the
// exact same pre-generated op stream, so differences in the result are
// about the engines, not about different random inputs.
//
// Latencies are stored as integer nanoseconds, not double: steady_clock's
// resolution doesn't justify sub-nanosecond precision, and integer
// formatting is far cheaper than float formatting when writing millions of
// samples to the output file (see profile_main.cpp and docs/DESIGN.md for
// how much this mattered while profiling).
//
// Configuration via environment variables:
//   BENCH_SEED         seed for reproduction (default: random)
//   BENCH_OPS          operations to measure (default: 1000000)
//   BENCH_WARMUP_OPS   operations to run unmeasured first (default: 50000)
//   BENCH_OUTPUT_DIR   directory for raw per-op latencies, one ns value
//                      per line, written as latencies_v1.csv/latencies_v2.csv
//                      (default: bench/results)

#include "flow_gen.hpp"
#include "orderbook/fast_order_book.hpp"
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
using namespace orderbook_bench;
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

std::string outputDirFromEnvOrDefault() {
    if (const char* s = std::getenv("BENCH_OUTPUT_DIR")) return s;
    return "bench/results";
}

std::int64_t percentile(const std::vector<std::int64_t>& sortedNs, double p) {
    if (sortedNs.empty()) return 0;
    std::size_t idx = static_cast<std::size_t>(p / 100.0 * static_cast<double>(sortedNs.size() - 1));
    return sortedNs[idx];
}

template <typename Book>
void runBenchmark(const char* label, const std::vector<GeneratedOp>& ops, std::size_t warmupCount,
                   std::size_t opCount, std::uint64_t seed, const std::string& outputDir) {
    Book book;

    std::fprintf(stderr, "orderbook bench [%s]: warming up (%zu ops)...\n", label, warmupCount);
    for (std::size_t i = 0; i < warmupCount; ++i) {
        apply(book, ops[i]);
    }

    std::fprintf(stderr, "orderbook bench [%s]: measuring (%zu ops)...\n", label, opCount);
    std::vector<std::int64_t> latenciesNs;
    latenciesNs.reserve(opCount);

    Clock::time_point start = Clock::now();
    for (std::size_t i = warmupCount; i < warmupCount + opCount; ++i) {
        Clock::time_point opStart = Clock::now();
        apply(book, ops[i]);
        Clock::time_point opEnd = Clock::now();
        latenciesNs.push_back(std::chrono::duration_cast<std::chrono::nanoseconds>(opEnd - opStart).count());
    }
    Clock::time_point end = Clock::now();

    double totalSeconds = std::chrono::duration<double>(end - start).count();
    double throughput = static_cast<double>(opCount) / totalSeconds;

    std::vector<std::int64_t> sorted = latenciesNs;
    std::sort(sorted.begin(), sorted.end());

    std::printf("\norderbook_bench %s results\n", label);
    std::printf("  seed:          %llu\n", static_cast<unsigned long long>(seed));
    std::printf("  ops measured:  %zu (warmup %zu)\n", opCount, warmupCount);
    std::printf("  wall time:     %.3f s\n", totalSeconds);
    std::printf("  throughput:    %.0f ops/sec\n", throughput);
    std::printf("  p50 latency:   %lld ns\n", static_cast<long long>(percentile(sorted, 50)));
    std::printf("  p99 latency:   %lld ns\n", static_cast<long long>(percentile(sorted, 99)));
    std::printf("  p99.9 latency: %lld ns\n", static_cast<long long>(percentile(sorted, 99.9)));

    std::filesystem::path outPath = std::filesystem::path(outputDir) / (std::string("latencies_") + label + ".csv");
    std::filesystem::create_directories(outPath.parent_path());
    std::ofstream out(outPath);
    out << "latency_ns\n";
    for (std::int64_t v : latenciesNs) out << v << "\n";
    std::fprintf(stderr, "orderbook bench [%s]: raw latencies written to %s\n", label, outPath.c_str());
}

} // namespace

int main() {
    std::uint64_t seed = seedFromEnvOrRandom();
    std::size_t opCount = sizeFromEnvOrDefault("BENCH_OPS", 1'000'000);
    std::size_t warmupCount = sizeFromEnvOrDefault("BENCH_WARMUP_OPS", 50'000);
    std::string outputDir = outputDirFromEnvOrDefault();

    std::fprintf(stderr, "orderbook bench: generating %zu ops (seed=%llu)...\n",
                 warmupCount + opCount, static_cast<unsigned long long>(seed));
    std::vector<GeneratedOp> ops = generateOps(warmupCount + opCount, seed);

    runBenchmark<OrderBook>("v1", ops, warmupCount, opCount, seed, outputDir);
    runBenchmark<FastOrderBook>("v2", ops, warmupCount, opCount, seed, outputDir);

    return 0;
}
