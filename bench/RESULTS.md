# Benchmark results

For the current, trustworthy numbers, see the table in
[../README.md](../README.md) or the "Current results" section below.
Everything past that is the detailed historical record: every run, every
methodology fix, kept rather than cleaned up, since an honest record is
more useful than a tidy one. See [../docs/HISTORY.md](../docs/HISTORY.md)
for the narrative version of the same story.

## Current results

Final op mix (45% limit / 20% cancel / 15% reduceQty / 15% replacePrice
/ 5% market), after all fixes below: v1 ~18.4M ops/sec (p50 ~67ns, p99
~176ns, p99.9 ~294ns), v2 ~25.5M ops/sec (p50 ~49ns, p99 ~151ns, p99.9
~257ns). Same hardware/build as every table below (Intel Core i7-13620H,
WSL2, `cmake --preset release`, GCC 15.2.0, `-O3 -DNDEBUG`).

Sparse-book scenario (`orderbook_sparse_bench`, one resting order far
from the action, see "Sparse book" below for what this tests): v1 ~45ns,
v2 ~54ns per add+cancel, both fast, neither pathological. Independently
re-verified on different hardware: 104ns (v2) vs 146ns (v1), v2 now
faster than v1 on its own former worst case.

Real NASDAQ ITCH 5.0 AAPL flow, a partial-day sample
(`orderbook_itch_replay`, 170,839 real ops, see `docs/HISTORY.md` for
the protocol mapping and the price-band filter this needed): v1 ~15M
ops/sec, v2 ~34M ops/sec, a bigger v2 advantage (~2.3x) than the
synthetic benchmark above (~1.4x), consistent with this real sample
touching ~20x more distinct price levels (5,517 vs 273), which costs
v1's O(log P) `std::map` lookup more and costs v2's O(1) design
nothing.

Concurrency (`orderbook_concurrency_bench`, gateway thread -> lock-free
SPSC queue -> matching thread, pinned to two distinct physical cores,
see `docs/HISTORY.md` for the full story including a hyperthread-sibling
pinning mistake): saturated (gateway sends flat-out), p50 scales almost
exactly linearly with queue depth, ~7µs at depth 64 vs ~440µs at depth
4096, a bounded queue's Little's Law behavior under sustained overload,
not a fixed handoff cost; a batched variant (one atomic store per batch
of 64 instead of per message) recovers a real chunk of the saturated
throughput drop (~26M single-threaded down to ~6-7M unbatched pipelined,
~9-10M batched). Paced at a fixed offered load well under capacity
instead (1M/4M/7M msgs/sec) isolates the true cross-core handoff cost:
p50 ~260-375ns at 1M and 4M (comfortably sustained), jumping back to
saturated-queue latency at 7M once the matching thread's real capacity
is exceeded and the queue starts backing up.

## index_ replaced with unordered_dense

`index_` (flagged by profiling as the biggest remaining cost in both
engines) switched from
`std::unordered_map` to `ankerl::unordered_dense::map`, a drop-in,
contiguous-storage hash map. Full story in `docs/HISTORY.md`, including a
detour where the first wall-clock benchmark after this change showed v1
getting *slower*, which an isolated A/B test disproved as a real
regression (reverting to `std::unordered_map` made it slower still), the
actual cause was session-level thermal throttling after many hours of
continuous rebuilding, not this code change.

**Trustworthy numbers (perf, percentage of engine time, self-normalizing
against clock speed, unlike wall-clock ops/sec measured hours apart):**

| | `index_` cost | Allocator overhead |
|---|---|---|
| v1, before | ~13.5% | ~17.3% |
| v1, after | ~3.25% | ~10.75% |
| v2, before | ~11.7% | ~5% |
| v2, after | ~3.5% | ~0.04% (essentially eliminated) |

**Wall-clock numbers (6 runs, post-cooldown, still noisier than earlier
sessions, read as indicative, not precise):** v1 ~14.9M ops/sec, p50
~86ns, p99 ~255ns, p99.9 ~438ns. v2 ~20.1M ops/sec, p50 ~66ns, p99
~214ns, p99.9 ~374ns. Both engines moved on every metric relative to the
"v1/v2 (corrected)" table below, consistent with a session-wide slowdown
affecting both equally rather than a change specific to one engine.
Not used as the headline result for this follow-up, the perf percentages
above are; included here for completeness and because hiding a
noisy measurement would be worse than labeling it noisy.

## Throughput was still contaminated by clock-call overhead

Reporting clock-call overhead directly, and timing whole batches rather
than summing per-op samples, makes the numbers more honest. The
throughput number already came from a single start/end wrapped around
the whole measured loop, not a sum of per-op samples, but that loop's
body still contained the per-op `Clock::now()` calls used for the
latency samples, so the "whole batch" timer was itself measuring time
spent in clock calls, not just engine work.

**Fix:** throughput and latency are now measured in two separate passes,
each against its own fresh book instance replaying the identical
pre-generated op stream. The throughput pass has zero per-op
instrumentation (`measureThroughput` in `bench/bench_main.cpp`); the
latency pass keeps the per-op timestamps needed for percentiles. Clock
overhead itself is measured once (calling `Clock::now()` back to back
200,000 times) and printed with every run, so the known floor under the
latency numbers is stated, not hidden.

**The effect was large, not cosmetic:**

| | Old (contaminated) | New (clean) | Change |
|---|---|---|---|
| v1 throughput | ~10.05M ops/sec | ~16.69M ops/sec | +66% |
| v2 throughput | ~11.34M ops/sec | ~20.44M ops/sec | +75% |

Clock-call overhead measured at ~16-24 ns per call, ~33-47 ns per
latency sample (two calls bracketing each op). At v2's ~58 ns p50, over
half of that was clock overhead, not engine work. The old numbers in the
sections below are left as they were originally measured and reported,
the point of this file is an honest record, not a cleaned-up one, but
they should not be trusted as absolute throughput figures. The new
methodology's numbers are in "v1/v2 (corrected)" below.

## v1/v2 (corrected)

Same hardware (Intel Core i7-13620H, WSL2), same build
(`cmake --preset release`, GCC 15.2.0, `-O3 -DNDEBUG`), same op mix and
pre-generated stream as below, new two-pass methodology.

**Results (4 runs, different random seeds):**

| Run | Engine | Throughput (ops/sec) | p50 (ns) | p99 (ns) | p99.9 (ns) | Clock overhead (ns/call) |
|-----|--------|----------------------|----------|----------|------------|---------------------------|
| 1   | v1     | 15,715,028           | 74       | 219      | 355        | 23.6                      |
| 1   | v2     | 19,812,084           | 59       | 207      | 344        | 23.6                      |
| 2   | v1     | 16,953,246           | 72       | 212      | 331        | 18.8                      |
| 2   | v2     | 20,886,014           | 58       | 202      | 312        | 18.8                      |
| 3   | v1     | 17,057,209           | 70       | 202      | 310        | 16.4                      |
| 3   | v2     | 20,885,553           | 57       | 198      | 311        | 16.4                      |
| 4   | v1     | 17,028,464           | 70       | 209      | 339        | 16.6                      |
| 4   | v2     | 20,174,433           | 57       | 197      | 305        | 16.6                      |

Averages: v1 ~16.69M ops/sec, p50 71.5ns, p99 210.5ns, p99.9 333.8ns.
v2 ~20.44M ops/sec, p50 57.75ns, p99 201ns, p99.9 318ns.

**v2 is about 22% higher throughput and 19% lower p50** than v1 under
this corrected methodology, both numbers changed from the original
~13%/~24% because removing a roughly-constant contamination source from
both measurements doesn't preserve their ratio. This is itself worth
knowing: a flawed measurement methodology doesn't just inflate absolute
numbers, it can distort the comparison the whole exercise exists to make.

## match() no longer allocates a vector per call

`match()` returned `std::vector<Trade>` by value, a fresh heap
allocation on every call even when most calls produce zero or one
trade. Fixed with a reused member buffer (`tradeBuffer_`); see
`docs/HISTORY.md` for the change itself.

**This specific benchmark's numbers barely moved** (within normal
run-to-run noise): the per-op `steady_clock::now()` overhead already
documented below dominates at this scale, so a few nanoseconds saved on
an allocation that often doesn't even happen (zero-trade ops are common)
doesn't show up clearly in p50/throughput here. The real evidence is in
the `perf` profile: profiling `FastOrderBook` directly for the first
time, total allocator overhead is about 5% of engine time, with
`index_`'s `unordered_map` operations (about 11.7%) now clearly the
dominant remaining cost, more than double the allocator. Full numbers in
`docs/HISTORY.md`.

## v1 (milestone 4, original measurement, since corrected above)

**Superseded by "v1/v2 (corrected)" above**, kept for an honest record of
what was actually measured and reported at the time, not retroactively
cleaned up. Do not use these throughput numbers; the methodology that
produced them was contaminated by clock-call overhead (see the
methodology-fix section above).

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
  `docs/DESIGN.md`), so profiling uses `perf`'s `task-clock`-based
  sampling instead of cycle-accurate profiling.

## v2 (milestone 5, original measurement, since corrected above)

**Also superseded by "v1/v2 (corrected)" above**, same reason.

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

Profiling found three engine-side cost centers: `malloc`/`cfree` and
allocator internals (~17% of engine time), `std::map` red-black tree
operations (~3.85%), and `index_`'s `unordered_map` operations (~13.5%).
`FastOrderBook`'s flat array + intrusive list + object pool directly
targets the first two (~21% combined), by design it does **not** touch
`index_` at this point, which stays an `unordered_map` in v2 here (see
docs/DESIGN.md for the current state; it's since been replaced, see
"index_ replaced with unordered_dense" above). So the ~13-24% measured
improvement lines up with removing roughly that ~21% slice of engine
time while the ~13.5% `index_` cost rides along unchanged, not a
shortfall against the profiling data, confirmation of it. The biggest
remaining opportunity for
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
`docs/HISTORY.md`):

```
sparse_bench [v1]: 48 ns per add+cancel (2000 iters)
sparse_bench [v2]: 41 ns per add+cancel (2000 iters)
```

v2 is now on par with or faster than v1 on this exact scenario, not
8,000x slower. This benchmark is permanent (`bench/sparse_bench_main.cpp`,
not a one-off script) specifically so this can't silently regress again.
