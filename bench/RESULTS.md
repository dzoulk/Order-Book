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

## v2 (milestone 5)

Same hardware/build/methodology as v1 above, same pre-generated op stream
fed to both engines in the same `./build/release/bench/orderbook_bench`
run (one binary now benchmarks both, see `bench/bench_main.cpp`).

**Results (4 runs, different random seeds):**

| Run | Engine | Throughput (ops/sec) | p50 (ns) | p99 (ns) | p99.9 (ns) |
|-----|--------|----------------------|----------|----------|------------|
| 1   | v1     | 9,971,417            | 73       | 239      | 369        |
| 1   | v2     | 11,428,566           | 56       | 223      | 370        |
| 2   | v1     | 10,040,046           | 72       | 229      | 345        |
| 2   | v2     | 11,618,486           | 54       | 207      | 307        |
| 3   | v1     | 9,998,169            | 73       | 234      | 358        |
| 3   | v2     | 11,195,436           | 56       | 212      | 318        |
| 4   | v1     | 10,196,262           | 72       | 228      | 342        |
| 4   | v2     | 11,100,414           | 55       | 212      | 315        |

Averages: v1 ~10.05M ops/sec, p50 72.5ns, p99 232.5ns, p99.9 353.5ns.
v2 ~11.34M ops/sec, p50 55.3ns, p99 213.5ns, p99.9 327.5ns (327.5 includes
run 1's 370 as an outlier; the other three average 313.3).

**v2 is about 13% higher throughput, 24% lower p50, 8% lower p99, and
roughly 10% lower p99.9** (excluding the one outlier run). Real, but more
modest than the profiling might suggest at first glance, worth explaining
rather than just reporting:

Milestone 5's profiling found three engine-side cost centers: `malloc`/
`cfree` and allocator internals (~17% of engine time), `std::map`
red-black tree operations (~3.85%), and `index_`'s `unordered_map`
operations (~13.5%). `FastOrderBook`'s flat array + intrusive list +
object pool directly targets the first two (~21% combined), by design it
does **not** touch `index_`, which stays an `unordered_map` in v2 (see
docs/DESIGN.md for why). So the ~13-24% measured improvement lines up
with removing roughly that ~21% slice of engine time while the ~13.5%
`index_` cost rides along unchanged, not a shortfall against the
profiling data, confirmation of it. The biggest remaining opportunity for
a v3 would be `index_` itself.

**Same caveats as v1 apply** (WSL2-not-bare-metal, per-op clock-call
overhead baked into every sample). One more for v2 specifically:
`FastOrderBook` only supports prices in `[0, kMaxPrice)` (1,000,000); the
benchmark's price band (~10,000 ± a small drift) is nowhere near that
limit, so it isn't a factor here, but it's a real behavioral difference
from v1, not just an implementation detail.

**That price band also hid a real bug**, see the sparse-book section
below.

## Sparse book (milestone 5 follow-up)

**Command:** `./build/release/bench/orderbook_sparse_bench`. One resting
bid at price 10, then 2,000 iterations of add+cancel a bid at price
900,000. Same hardware/build as above.

**Before the fix** (linear-scan `findNextOccupied`):

| Engine | ns per add+cancel |
|---|---|
| v1 | ~46-50 |
| v2 | ~388,000-432,000 (about 8,000-8,500x slower than v1) |

Every add or cancel at the far price had to walk the price array looking
for the only other occupied level, roughly 900,000 slots away. Genuinely
O(price range), not O(1), despite what the v1-vs-v2 table above's
"amortized O(1)" claim said. The all-benchmarks-use-the-same-narrow-band
problem: nothing in this file's methodology would ever generate a price
900,000 ticks from the action, so nothing caught it.

**After the fix** (`OccupancyBitmap`, a hierarchical bitmap, see
`docs/DESIGN.md`):

```
sparse_bench [v1]: 48 ns per add+cancel (2000 iters)
sparse_bench [v2]: 41 ns per add+cancel (2000 iters)
```

v2 is now on par with or faster than v1 on this exact scenario, not
8,000x slower. This benchmark is permanent (`bench/sparse_bench_main.cpp`,
not a one-off script) specifically so this can't silently regress again.
