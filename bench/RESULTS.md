# Benchmark results

## v1 (milestone 4)

**Hardware:** Intel Core i7-13620H (8 cores / 16 threads), 7.6 GiB RAM
visible to the VM, running inside WSL2 (Ubuntu 26.04, kernel
6.6.87.2-microsoft-standard-WSL2), not bare-metal Linux.

**Build:** `cmake --preset release` (`CMAKE_BUILD_TYPE=Release`, GCC 15.2.0,
`-O3 -DNDEBUG`). Command: `./build/release/bench/orderbook_bench`.

**Methodology:** 50,000 warmup ops (unmeasured) followed by 1,000,000
measured ops, mix 60% limit / 30% cancel / 10% market, prices clustered in
a ±10 tick band around a midpoint that drifts by ±1 tick roughly 5% of the
time. The full operation stream is pre-generated before timing starts, so
RNG/distribution-sampling cost never lands inside a measured latency
sample; only the `OrderBook` method call itself is timed, with
`std::chrono::steady_clock`.

**Results (4 runs, different random seeds):**

| Run | Throughput (ops/sec) | p50 (ns) | p99 (ns) | p99.9 (ns) |
|-----|----------------------|----------|----------|------------|
| 1   | 9,731,482            | 72       | 240      | 387        |
| 2   | 9,850,580            | 73       | 234      | 366        |
| 3   | 9,425,051            | 73       | 241      | 389        |
| 4   | 10,099,671           | 69       | 225      | 346        |

Roughly **9.4-10.1M ops/sec**, **p50 ~70-75ns**, **p99 ~225-240ns**,
**p99.9 ~350-390ns**. Stable across runs, no outlier runs discarded.

**Caveats, read before trusting these numbers for anything beyond relative
v1-vs-v2 comparison:**
- **WSL2, not bare metal.** These numbers reflect a VM on top of Windows,
  not a dedicated Linux box. Expect some overhead and noise versus running
  directly on Linux hardware.
- **Per-op `steady_clock::now()` overhead is included in every latency
  sample.** At tens of nanoseconds of actual engine work, the clock call
  itself (tens of ns on this platform) is a non-trivial fraction of what's
  being measured. This mostly affects the absolute latency numbers, not
  the v1-vs-v2 comparison, since the same overhead applies to both.
- **Hardware PMU counters aren't available under this WSL2 kernel** (see
  `docs/DESIGN.md`), so milestone 5 profiling will use `perf`'s
  `task-clock`-based sampling instead of cycle-accurate profiling.

## v2

TODO, milestone 5, after profiling v1 with `perf` and applying the
optimizations in `docs/DESIGN.md`.
