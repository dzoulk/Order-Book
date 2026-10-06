// Concurrency: a gateway thread handing orders off through a lock-free
// SPSC ring buffer (include/orderbook/spsc_queue.hpp) to a matching
// thread that owns FastOrderBook, instead of calling the book directly
// in-process. Measures end-to-end latency: from the moment the gateway
// thread timestamps an order, through the other core noticing it, to
// the moment the matching thread has actually applied it to the book.
//
// A single-threaded baseline (the exact same pre-generated op stream,
// same process, measured immediately before the pipelined run) is
// reported alongside the pipelined numbers, rather than compared
// against a figure from some separate earlier run: this project already
// learned that cross-run wall-clock comparisons taken hours apart aren't
// trustworthy (see docs/HISTORY.md, "index_ replaced with
// unordered_dense"), so the two numbers that matter here are measured
// back to back in one process.
//
// Only FastOrderBook (v2) runs through the pipeline. This benchmark is
// about what the queue/thread handoff costs on top of the engine, not
// re-litigating v1 vs v2.
//
// This benchmark measures two different things, deliberately kept
// separate rather than conflated into one number:
//   - Saturated runs (shallow queue, deep queue, batched): the gateway
//     sends flat out, no delay between messages, so these measure
//     queueing behavior under sustained overload, not handoff cost.
//   - Paced runs: the gateway sends at a fixed offered load well under
//     the matching thread's service rate, so these isolate the true
//     cross-core handoff cost, with the queue staying close to empty.
// A single "p50 latency" number without saying which of these a
// benchmark measured would be close to meaningless; see docs/HISTORY.md
// for the measured numbers and why they differ so much.
//
// Three things turned out to matter enough to build into this benchmark
// rather than just footnote (full story in docs/HISTORY.md):
//
// 1. Thread placement. On a hybrid P-core/E-core CPU, two logical CPUs
//    can be hyperthread siblings of the *same* physical core; pinning
//    the gateway and matching threads to a sibling pair made this
//    benchmark's p50 latency about 6x worse than pinning them to two
//    genuinely distinct physical cores, same code, same machine. This
//    benchmark reads /sys/devices/system/cpu/*/topology/core_id and
//    picks two logical CPUs on different physical cores automatically,
//    rather than assuming adjacent CPU numbers are distinct cores.
// 2. Queue depth under sustained saturation. When the gateway's enqueue
//    rate is at or above the matching thread's service rate, a bounded
//    SPSC queue settles into a "mostly full" steady state: by Little's
//    Law, every item then waits for roughly (queue depth / consumer
//    rate), not some fixed handoff cost. This isn't a bug or environment
//    noise, it reproduces with a trivial atomic ping-pong too, it's an
//    inherent property of a saturated bounded queue, so this benchmark
//    runs the identical op stream through both a shallow and a deep
//    queue to show latency scaling with depth directly, the real
//    throughput/latency tradeoff a queue-depth config knob makes in a
//    real gateway.
// 3. Per-message cross-core traffic caps saturated throughput well
//    below the single-threaded baseline. Every tryPush/tryPop does one
//    atomic store the other core has to notice; SpscQueue's batch API
//    (tryPushBatch/tryPopBatch) amortizes that store over many messages
//    instead of paying it per message. The batched saturated run tests
//    this directly against the single-item saturated runs above it.
//
// Configuration via environment variables:
//   CONCURRENCY_BENCH_SEED   seed for reproduction (default: random)
//   CONCURRENCY_BENCH_OPS    operations to measure (default: 500000)

#include "flow_gen.hpp"
#include "orderbook/fast_order_book.hpp"
#include "orderbook/spsc_queue.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <random>
#include <thread>
#include <vector>

#ifdef __linux__
#include <fstream>
#include <pthread.h>
#include <sched.h>
#include <unistd.h>
#endif

using namespace orderbook;
using namespace orderbook_bench;
using Clock = std::chrono::steady_clock;

namespace {

std::uint64_t seedFromEnvOrRandom() {
    if (const char* s = std::getenv("CONCURRENCY_BENCH_SEED")) return std::strtoull(s, nullptr, 10);
    std::random_device rd;
    return (static_cast<std::uint64_t>(rd()) << 32) | rd();
}

std::size_t sizeFromEnvOrDefault(const char* name, std::size_t fallback) {
    if (const char* s = std::getenv(name)) return static_cast<std::size_t>(std::strtoull(s, nullptr, 10));
    return fallback;
}

std::int64_t percentile(const std::vector<std::int64_t>& sortedNs, double p) {
    if (sortedNs.empty()) return 0;
    std::size_t idx = static_cast<std::size_t>(p / 100.0 * static_cast<double>(sortedNs.size() - 1));
    return sortedNs[idx];
}

// Finds two logical CPUs that sit on two different physical cores, by
// reading each online CPU's core_id out of sysfs, rather than assuming
// adjacent CPU numbers are distinct cores (on this project's own
// development machine, a hybrid P-core/E-core laptop CPU, CPU 0 and CPU
// 1 are hyperthread siblings of the *same* physical core). Returns
// nullopt if topology can't be read (non-Linux, or a sandboxed
// environment without sysfs), in which case the caller runs unpinned.
std::optional<std::pair<int, int>> pickTwoCpusOnDistinctCores() {
#ifdef __linux__
    long nCpus = sysconf(_SC_NPROCESSORS_ONLN);
    if (nCpus < 2) return std::nullopt;

    std::vector<int> coreIdOf(static_cast<std::size_t>(nCpus), -1);
    for (long cpu = 0; cpu < nCpus; ++cpu) {
        std::ifstream f("/sys/devices/system/cpu/cpu" + std::to_string(cpu) + "/topology/core_id");
        int id = -1;
        if (f >> id) coreIdOf[static_cast<std::size_t>(cpu)] = id;
    }
    for (long i = 0; i < nCpus; ++i) {
        if (coreIdOf[static_cast<std::size_t>(i)] < 0) continue;
        for (long j = i + 1; j < nCpus; ++j) {
            if (coreIdOf[static_cast<std::size_t>(j)] >= 0 &&
                coreIdOf[static_cast<std::size_t>(j)] != coreIdOf[static_cast<std::size_t>(i)]) {
                return std::make_pair(static_cast<int>(i), static_cast<int>(j));
            }
        }
    }
    return std::nullopt;
#else
    return std::nullopt;
#endif
}

bool pinThreadToCpu(std::thread& t, int cpu) {
#ifdef __linux__
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    return pthread_setaffinity_np(t.native_handle(), sizeof(set), &set) == 0;
#else
    (void)t;
    (void)cpu;
    return false;
#endif
}

bool pinSelfToCpu(int cpu) {
#ifdef __linux__
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    return pthread_setaffinity_np(pthread_self(), sizeof(set), &set) == 0;
#else
    (void)cpu;
    return false;
#endif
}

// Same measurement as bench_main.cpp's helper of the same name: the
// cost of a single Clock::now() call, back to back. A baseline latency
// sample brackets one op with two of these; a pipeline sample has one
// on the gateway thread and one on the matching thread, so this is a
// rough floor under both, not an exact one.
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

// Uninstrumented throughput pass, no per-op timestamps, same pattern
// bench_main.cpp uses: a single start/end around the whole loop, on its
// own fresh book instance.
double measureBaselineThroughput(const std::vector<GeneratedOp>& ops) {
    FastOrderBook book;
    Clock::time_point start = Clock::now();
    for (const GeneratedOp& op : ops) apply(book, op);
    Clock::time_point end = Clock::now();
    return static_cast<double>(ops.size()) / std::chrono::duration<double>(end - start).count();
}

// Per-op latency pass, single-threaded, in-process: the number every
// other benchmark in this project already reports, measured here too so
// the pipelined numbers below have a same-run, same-process baseline to
// compare against instead of a figure from elsewhere.
std::vector<std::int64_t> measureBaselineLatencies(const std::vector<GeneratedOp>& ops) {
    FastOrderBook book;
    std::vector<std::int64_t> latenciesNs;
    latenciesNs.reserve(ops.size());
    for (const GeneratedOp& op : ops) {
        Clock::time_point opStart = Clock::now();
        apply(book, op);
        Clock::time_point opEnd = Clock::now();
        latenciesNs.push_back(std::chrono::duration_cast<std::chrono::nanoseconds>(opEnd - opStart).count());
    }
    return latenciesNs;
}

struct GatewayMessage {
    GeneratedOp op;
    Clock::time_point enqueuedAt;
};

struct PipelineResult {
    std::vector<std::int64_t> latenciesNs;
    double throughput;  // from wall time around the whole run: the two
                         // threads overlap, so this isn't a sum of the
                         // per-op latency samples above it.
};

// Gateway thread hands every op off through the queue; matching thread
// applies each to its own FastOrderBook and timestamps right after, so
// the latency sample covers the full handoff: enqueue, the other core
// noticing it, and the book actually being updated. Pins both threads
// to the given CPUs if pinning is available (see
// pickTwoCpusOnDistinctCores above for why this matters).
//
// targetRateHz, if given, paces the gateway to that offered load
// (scheduled by an absolute next-send time, not "sleep(interval)" after
// each send, so timing jitter in one send doesn't compound into the
// next) instead of sending flat out. This isolates the true cross-core
// handoff cost from the queueing delay a saturated gateway adds (see
// the shallow/deep queue runs below for that part): at an offered load
// comfortably under the matching thread's service rate, the queue
// should stay almost empty, so each latency sample is close to a pure
// handoff cost, not queueing time.
PipelineResult measurePipeline(const std::vector<GeneratedOp>& ops, std::size_t queueCapacity,
                                std::optional<std::pair<int, int>> cpus,
                                std::optional<double> targetRateHz = std::nullopt) {
    SpscQueue<GatewayMessage> queue(queueCapacity);
    std::vector<std::int64_t> latenciesNs(ops.size());

    Clock::time_point pipelineStart = Clock::now();

    std::thread gateway([&] {
        Clock::duration interval{};
        Clock::time_point nextSend = Clock::now();
        if (targetRateHz) interval = std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(1.0 / *targetRateHz));
        for (const GeneratedOp& op : ops) {
            if (targetRateHz) {
                while (Clock::now() < nextSend) {
                    // not yet due, spin until the scheduled send time
                }
            }
            GatewayMessage msg{op, Clock::now()};
            while (!queue.tryPush(msg)) {
                // queue full, spin
            }
            if (targetRateHz) nextSend += interval;
        }
    });
    if (cpus) pinThreadToCpu(gateway, cpus->first);

    if (cpus) pinSelfToCpu(cpus->second);
    FastOrderBook book;
    std::size_t processed = 0;
    while (processed < ops.size()) {
        if (auto msg = queue.tryPop()) {
            apply(book, msg->op);
            Clock::time_point now = Clock::now();
            latenciesNs[processed] =
                std::chrono::duration_cast<std::chrono::nanoseconds>(now - msg->enqueuedAt).count();
            ++processed;
        }
    }
    gateway.join();

    Clock::time_point pipelineEnd = Clock::now();
    double throughput =
        static_cast<double>(ops.size()) / std::chrono::duration<double>(pipelineEnd - pipelineStart).count();
    return PipelineResult{std::move(latenciesNs), throughput};
}

// Same pipeline, but publishing through the queue's batch API
// (SpscQueue::tryPushBatch/tryPopBatch) instead of one tryPush/tryPop
// per message: both sides accumulate a local batch of batchSize before
// touching the queue, so head_/tail_ get one atomic store per batch
// instead of one per message. Only meaningful under saturation (a
// paced gateway rarely has a full batch ready), so this is run
// alongside the saturated shallow/deep queue measurements, not the
// paced ones, specifically to test whether per-message cross-core
// cache-line traffic explains the throughput drop from the
// single-threaded baseline (see docs/HISTORY.md for the measured
// answer).
PipelineResult measurePipelineBatched(const std::vector<GeneratedOp>& ops, std::size_t queueCapacity,
                                       std::size_t batchSize, std::optional<std::pair<int, int>> cpus) {
    SpscQueue<GatewayMessage> queue(queueCapacity);
    std::vector<std::int64_t> latenciesNs(ops.size());

    Clock::time_point pipelineStart = Clock::now();

    std::thread gateway([&] {
        std::vector<GatewayMessage> batch;
        batch.reserve(batchSize);
        auto flush = [&] {
            std::size_t written = 0;
            while (written < batch.size()) written += queue.tryPushBatch(batch.data() + written, batch.size() - written);
            batch.clear();
        };
        for (const GeneratedOp& op : ops) {
            batch.push_back(GatewayMessage{op, Clock::now()});
            if (batch.size() == batchSize) flush();
        }
        if (!batch.empty()) flush();
    });
    if (cpus) pinThreadToCpu(gateway, cpus->first);

    if (cpus) pinSelfToCpu(cpus->second);
    FastOrderBook book;
    std::vector<GatewayMessage> batch(batchSize);
    std::size_t processed = 0;
    while (processed < ops.size()) {
        std::size_t n = queue.tryPopBatch(batch.data(), batchSize);
        for (std::size_t i = 0; i < n; ++i) {
            apply(book, batch[i].op);
            Clock::time_point now = Clock::now();
            latenciesNs[processed] =
                std::chrono::duration_cast<std::chrono::nanoseconds>(now - batch[i].enqueuedAt).count();
            ++processed;
        }
    }
    gateway.join();

    Clock::time_point pipelineEnd = Clock::now();
    double throughput =
        static_cast<double>(ops.size()) / std::chrono::duration<double>(pipelineEnd - pipelineStart).count();
    return PipelineResult{std::move(latenciesNs), throughput};
}

void printStats(const char* label, const std::vector<std::int64_t>& latenciesNs, double throughput) {
    std::vector<std::int64_t> sorted = latenciesNs;
    std::sort(sorted.begin(), sorted.end());
    std::printf("\n%s\n", label);
    std::printf("  throughput:    %.0f ops/sec\n", throughput);
    std::printf("  p50 latency:   %lld ns\n", static_cast<long long>(percentile(sorted, 50)));
    std::printf("  p99 latency:   %lld ns\n", static_cast<long long>(percentile(sorted, 99)));
    std::printf("  p99.9 latency: %lld ns\n", static_cast<long long>(percentile(sorted, 99.9)));
}

} // namespace

int main() {
    std::uint64_t seed = seedFromEnvOrRandom();
    std::size_t opCount = sizeFromEnvOrDefault("CONCURRENCY_BENCH_OPS", 500'000);

    double clockOverheadNs = measureClockOverheadNs();
    std::printf("clock-call overhead: %.1f ns per Clock::now() call\n", clockOverheadNs);

    std::optional<std::pair<int, int>> cpus = pickTwoCpusOnDistinctCores();
    if (cpus) {
        std::fprintf(stderr, "concurrency bench: pinning gateway thread to cpu %d, matching thread to cpu %d\n",
                      cpus->first, cpus->second);
    } else {
        std::fprintf(stderr, "concurrency bench: could not determine CPU topology, running unpinned (expect "
                              "noisier numbers)\n");
    }

    std::fprintf(stderr, "concurrency bench: generating %zu ops (seed=%llu)...\n", opCount,
                 static_cast<unsigned long long>(seed));
    std::vector<GeneratedOp> ops = generateOps(opCount, seed);

    std::fprintf(stderr, "concurrency bench: measuring single-threaded baseline...\n");
    double baselineThroughput = measureBaselineThroughput(ops);
    std::vector<std::int64_t> baselineLatencies = measureBaselineLatencies(ops);
    printStats("baseline (single-threaded, in-process)", baselineLatencies, baselineThroughput);

    constexpr std::size_t kShallowQueue = 64;
    constexpr std::size_t kDeepQueue = 4096;

    std::fprintf(stderr, "concurrency bench: measuring pipeline, shallow queue (capacity %zu)...\n",
                 kShallowQueue);
    PipelineResult shallow = measurePipeline(ops, kShallowQueue, cpus);
    printStats("pipelined, shallow queue (gateway -> SPSC queue -> matching thread)", shallow.latenciesNs,
               shallow.throughput);

    std::fprintf(stderr, "concurrency bench: measuring pipeline, deep queue (capacity %zu)...\n", kDeepQueue);
    PipelineResult deep = measurePipeline(ops, kDeepQueue, cpus);
    printStats("pipelined, deep queue (gateway -> SPSC queue -> matching thread)", deep.latenciesNs,
               deep.throughput);

    constexpr std::size_t kBatchSize = 64;
    std::fprintf(stderr, "concurrency bench: measuring pipeline, deep queue, batched (batch size %zu)...\n",
                 kBatchSize);
    PipelineResult batched = measurePipelineBatched(ops, kDeepQueue, kBatchSize, cpus);
    printStats("pipelined, deep queue, batched (same saturation, batch API)", batched.latenciesNs,
               batched.throughput);

    constexpr double kPacedRatesHz[] = {1'000'000.0, 4'000'000.0, 7'000'000.0};
    for (double rate : kPacedRatesHz) {
        std::fprintf(stderr, "concurrency bench: measuring pipeline, paced at %.0f msgs/sec...\n", rate);
        PipelineResult paced = measurePipeline(ops, kDeepQueue, cpus, rate);
        char label[128];
        std::snprintf(label, sizeof(label), "pipelined, paced at %.0f msgs/sec offered load", rate);
        printStats(label, paced.latenciesNs, paced.throughput);
    }

    return 0;
}
