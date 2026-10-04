// Throughput/latency benchmark, v1 (OrderBook) and v2 (FastOrderBook) in
// one run so the numbers are directly comparable. NaiveOrderBook is never
// benchmarked here: it's O(n) by design and exists purely as a
// correctness oracle for the fuzz test, not a baseline to beat.
//
// The operation stream is fully pre-generated before timing starts, so RNG
// and distribution-sampling overhead never lands inside a measured latency
// sample. Both engines run the exact same pre-generated op stream, so
// differences in the result are about the engines, not about different
// random inputs.
//
// Throughput and per-op latency are measured in two separate passes, each
// against its own fresh book instance replaying the identical op stream:
//   - Throughput pass: a single start/end around the whole measured
//     region, no per-op instrumentation at all. This is the number
//     reported as "throughput".
//   - Latency pass: timestamps before and after every single op, to get
//     a full distribution for percentiles. Wrapping a single start/end
//     around *this* loop would still be contaminated by the per-op
//     clock_gettime calls inside it, which is why throughput comes from
//     the separate, uninstrumented pass instead.
// Clock-call overhead is measured once and reported alongside the
// results: every latency sample includes roughly two of those calls
// (one before the op, one after), so it's a real, known floor under the
// smallest latencies, not a mystery.
//
// Latencies are stored as integer nanoseconds, not double: steady_clock's
// resolution doesn't justify sub-nanosecond precision, and integer
// formatting is far cheaper than float formatting when writing millions of
// samples to the output file (see profile_main.cpp and docs/HISTORY.md
// for how much this mattered while profiling).
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

// Cost of a single Clock::now() call, measured by calling it back to
// back many times. Each latency sample in the per-op loop below brackets
// the measured call with two of these, so roughly 2x this value is a
// floor under every latency number, not engine work.
double measureClockOverheadNs() {
    constexpr int kWarmup = 1000;
    constexpr int kCalls = 200'000;
    for (int i = 0; i < kWarmup; ++i) {
        volatile auto t = Clock::now();
        (void)t;
    }
    Clock::time_point start = Clock::now();
    for (int i = 0; i < kCalls; ++i) {
        volatile auto t = Clock::now();
        (void)t;
    }
    Clock::time_point end = Clock::now();
    return std::chrono::duration<double, std::nano>(end - start).count() / kCalls;
}

// Clean throughput measurement: no per-op instrumentation, a single
// start/end around the whole measured region, against a fresh book
// instance so it's unaffected by whatever the latency pass did.
template <typename Book>
double measureThroughput(const std::vector<GeneratedOp>& ops, std::size_t warmupCount, std::size_t opCount) {
    Book book;
    for (std::size_t i = 0; i < warmupCount; ++i) apply(book, ops[i]);

    Clock::time_point start = Clock::now();
    for (std::size_t i = warmupCount; i < warmupCount + opCount; ++i) apply(book, ops[i]);
    Clock::time_point end = Clock::now();

    double totalSeconds = std::chrono::duration<double>(end - start).count();
    return static_cast<double>(opCount) / totalSeconds;
}

// Per-op latency distribution: a second, fresh book instance replays the
// same op stream with a timestamp around every individual op. The
// resulting latency samples include clock-call overhead (reported
// separately by measureClockOverheadNs); this pass's own wall time is
// not used for throughput, see measureThroughput above for why.
template <typename Book>
std::vector<std::int64_t> measureLatencies(const std::vector<GeneratedOp>& ops, std::size_t warmupCount,
                                            std::size_t opCount) {
    Book book;
    for (std::size_t i = 0; i < warmupCount; ++i) apply(book, ops[i]);

    std::vector<std::int64_t> latenciesNs;
    latenciesNs.reserve(opCount);
    for (std::size_t i = warmupCount; i < warmupCount + opCount; ++i) {
        Clock::time_point opStart = Clock::now();
        apply(book, ops[i]);
        Clock::time_point opEnd = Clock::now();
        latenciesNs.push_back(std::chrono::duration_cast<std::chrono::nanoseconds>(opEnd - opStart).count());
    }
    return latenciesNs;
}

template <typename Book>
void runBenchmark(const char* label, const std::vector<GeneratedOp>& ops, std::size_t warmupCount,
                   std::size_t opCount, std::uint64_t seed, const std::string& outputDir,
                   double clockOverheadNs) {
    std::fprintf(stderr, "orderbook bench [%s]: measuring throughput (%zu ops, uninstrumented)...\n", label,
                 opCount);
    double throughput = measureThroughput<Book>(ops, warmupCount, opCount);

    std::fprintf(stderr, "orderbook bench [%s]: measuring latency distribution (%zu ops)...\n", label, opCount);
    std::vector<std::int64_t> latenciesNs = measureLatencies<Book>(ops, warmupCount, opCount);

    std::vector<std::int64_t> sorted = latenciesNs;
    std::sort(sorted.begin(), sorted.end());

    std::printf("\norderbook_bench %s results\n", label);
    std::printf("  seed:          %llu\n", static_cast<unsigned long long>(seed));
    std::printf("  ops measured:  %zu (warmup %zu)\n", opCount, warmupCount);
    std::printf("  throughput:    %.0f ops/sec (uninstrumented pass)\n", throughput);
    std::printf("  p50 latency:   %lld ns (includes ~%.0fns clock-call overhead)\n",
                 static_cast<long long>(percentile(sorted, 50)), 2 * clockOverheadNs);
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

    double clockOverheadNs = measureClockOverheadNs();
    std::printf("clock-call overhead: %.1f ns per Clock::now() call (~%.1f ns per latency sample)\n",
                clockOverheadNs, 2 * clockOverheadNs);

    std::fprintf(stderr, "orderbook bench: generating %zu ops (seed=%llu)...\n",
                 warmupCount + opCount, static_cast<unsigned long long>(seed));
    std::vector<GeneratedOp> ops = generateOps(warmupCount + opCount, seed);

    runBenchmark<OrderBook>("v1", ops, warmupCount, opCount, seed, outputDir, clockOverheadNs);
    runBenchmark<FastOrderBook>("v2", ops, warmupCount, opCount, seed, outputDir, clockOverheadNs);

    return 0;
}
