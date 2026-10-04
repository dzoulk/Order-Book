// Minimal driver for `perf record`, deliberately with none of
// bench_main's instrumentation (no per-op clock_gettime, no sort, no
// result file write). Those turned out to dominate `perf`'s profile at
// realistic op counts (see docs/DESIGN.md milestone 5), swamping the
// actual OrderBook cost with benchmark-harness noise. This generates the
// operation stream once, then applies it in a tight loop with only a
// single start/end timestamp around the whole thing, so `perf` sees
// almost nothing but OrderBook/map/list/unordered_map code.
//
// Configuration via environment variables:
//   BENCH_SEED     seed for reproduction (default: random)
//   BENCH_OPS      operations to apply (default: 20000000)
//   BENCH_ENGINE   "v1" (OrderBook, default) or "v2" (FastOrderBook)

#include "flow_gen.hpp"
#include "orderbook/fast_order_book.hpp"
#include "orderbook/order_book.hpp"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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

std::size_t opsFromEnvOrDefault() {
    if (const char* s = std::getenv("BENCH_OPS")) return static_cast<std::size_t>(std::strtoull(s, nullptr, 10));
    return 20'000'000;
}

template <typename Book>
void profile(const char* label, const std::vector<GeneratedOp>& ops) {
    Book book;
    std::fprintf(stderr, "orderbook profile [%s]: applying %zu ops (this is what perf should see)...\n", label,
                 ops.size());
    Clock::time_point start = Clock::now();
    for (const GeneratedOp& op : ops) {
        apply(book, op);
    }
    Clock::time_point end = Clock::now();

    double totalSeconds = std::chrono::duration<double>(end - start).count();
    std::fprintf(stderr, "orderbook profile [%s]: done, %.3f s, %.0f ops/sec\n", label, totalSeconds,
                 static_cast<double>(ops.size()) / totalSeconds);
}

} // namespace

int main() {
    std::uint64_t seed = seedFromEnvOrRandom();
    std::size_t opCount = opsFromEnvOrDefault();
    const char* engine = std::getenv("BENCH_ENGINE");
    bool useV2 = engine && std::strcmp(engine, "v2") == 0;

    std::fprintf(stderr, "orderbook profile: generating %zu ops (seed=%llu)...\n", opCount,
                 static_cast<unsigned long long>(seed));
    std::vector<GeneratedOp> ops = generateOps(opCount, seed, /*includeModifyOps=*/false);

    if (useV2) {
        profile<FastOrderBook>("v2", ops);
    } else {
        profile<OrderBook>("v1", ops);
    }
    return 0;
}
