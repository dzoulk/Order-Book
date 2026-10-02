# Design Notes

Tracks design decisions and rationale as the project progresses. Sections
get filled in as the milestone they belong to is completed.

## Milestones

- [x] 1. Scaffolding, CMake, CI, empty `OrderBook` interface, reference book
- [x] 2. v1 engine + unit tests passing
- [x] 3. Fuzz harness + sanitizers green
- [x] 4. Benchmark harness + v1 results
- [x] 5. Profile, v2 optimizations, v1 vs v2 results
- [x] 6. README polish

## Decisions made so far

- **Prices are integer ticks (`Price = int64_t`), never floating point.**
  Avoids binary-fraction rounding errors in price comparisons and arithmetic.
- **Time priority uses a monotonic sequence number (`SeqNum`), not wall-clock
  timestamps.** Clock resolution/monotonicity isn't guaranteed across calls;
  a counter is simpler and deterministic for the differential fuzz test.
- **Invalid input (`qty == 0`, duplicate id) throws `std::invalid_argument`.**
  These are programmer errors, distinct from `cancel`'s expected-to-sometimes-
  fail `bool` return for an unknown id.
- **"Duplicate id" means an order with that id is currently resting.** Once
  an id has fully filled or been cancelled, it can be reused.
- **`NaiveOrderBook` (tests/reference) intentionally uses linear scans over a
  flat `std::vector<Order>`.** It exists to be obviously correct, not fast.
  The milestone 3 fuzz test diffs the real `OrderBook` against it.
- **Sanitizers (ASan/UBSan) run in CI on Linux only.** UBSan isn't well
  supported on MSVC; the Windows CI job is a plain build+test smoke check.
- **Local development happens in WSL2 (Ubuntu), not native Windows.** `perf`
  doesn't run on Windows, UBSan isn't supported on MSVC, and ASan is limited
  there too. The project lives at `~/order-book` inside the WSL2 Linux
  filesystem (not `/mnt/c/...`) since builds and benchmarks across the
  Windows/Linux filesystem boundary are dramatically slower and would
  distort measurements. See the Toolchain section below.

## Toolchain (verified 2026-09-30)

Set up in WSL2 Ubuntu 26.04: `build-essential clang cmake ninja-build gdb
linux-tools-generic`. All three presets (`debug`, `release`, `sanitize`)
configure, build, and pass `ctest` cleanly in this environment.

**`perf` caveat:** hardware PMU counters (`cycles`, `instructions`,
`cache-misses`) aren't available under the WSL2 kernel. `perf stat -e cycles`
fails with "Unable to find PMU". Software events work fine though
(`perf stat -e task-clock,context-switches,page-faults`, and
`perf record -e task-clock -g` for call-graph sampling). For milestone 5
profiling, use `task-clock`-based sampling instead of cycle-based. That's
enough to find which functions are hot, which is the actual goal.

## Fuzz harness (milestone 3)

`tests/fuzz_test.cpp` runs the same random stream of operations against
`NaiveOrderBook` and `OrderBook` and checks they agree after every single
op, both on returned trades and on full book state (bestBid, bestAsk, and
depth at every price in the generated band). A mismatch prints the seed and
the failing op index so it can be reproduced with
`ORDERBOOK_FUZZ_SEED=<seed> ORDERBOOK_FUZZ_OPS=<n>`.

Default op count is 20,000, which runs in about a second, so it doesn't
slow down routine local test runs. CI pins it to the same value explicitly.
Verified at 5,000,000 ops (v1 only) and, after milestone 5 added
`FastOrderBook`, 500,000 ops (v1 and v2 together) with zero mismatches.

**Cost scales roughly quadratically with op count, not linearly.** 500k
ops took 149s (~298µs/op); the earlier 5M-op run took ~4.76ms/op, about
16x worse at 10x more ops. `NaiveOrderBook`'s `resting_` grows over a long
run, and `depthAt` is O(n), so every per-op state check gets slower as the
run goes on; total cost across a run is roughly O(n²) in op count. That's
an acceptable property of a deliberately-naive oracle, not a bug, it's
why 500k ops was treated as sufficient validation for `FastOrderBook`
rather than re-running the full 5M for both engines at once.

**Timing caveat found during that run:** the fuzz process reported about
6.6 hours of CPU time but almost 27 hours of wall-clock elapsed time (24%
average CPU), and the system clock rolled over a day during the run. The
WSL2 VM was clearly asleep, suspended, or heavily throttled for most of
that stretch rather than actually computing. The correctness result still
holds since it's independent of timing, but this is a real problem for
milestone 4: wall-clock `time` across a sleep/suspend is meaningless, and
benchmark runs will need either a guaranteed-awake machine for the
duration or a sanity check that CPU time and elapsed time actually match
before trusting a number.

## Benchmark harness (milestone 4)

`bench/bench_main.cpp` only benchmarks the real `OrderBook`, never
`NaiveOrderBook`: the naive book is O(n) by design and exists purely as a
correctness oracle for the fuzz test, not a baseline worth measuring.

The full operation stream (60% limit / 30% cancel / 10% market, prices in
a drifting band around a midpoint) is pre-generated before timing starts,
so RNG overhead never lands inside a measured latency sample, only the
`OrderBook` call itself is timed. Full results, methodology, and caveats
(WSL2-not-bare-metal, clock-call overhead inside each sample) are in
`bench/RESULTS.md`. Headline numbers: ~9.4-10.1M ops/sec, p50 ~70-75ns,
p99 ~225-240ns, p99.9 ~350-390ns, measured on an i7-13620H under WSL2.

This run completed in well under a second end to end, so the WSL2
sleep/suspend timing problem found during the milestone 3 fuzz run isn't a
concern here, there's no realistic window for the host to sleep mid-run.

## v1 `OrderBook` design (milestone 2)

- `bids_`: `std::map<Price, std::list<Order>, std::greater<Price>>`
- `asks_`: `std::map<Price, std::list<Order>>`
- `index_`: `std::unordered_map<OrderId, Location>` for O(1) cancel, where
  `Location` holds the side, price, and a `std::list<Order>::iterator`
- `std::list` iterators stay valid when other elements are erased or
  inserted, which is why `index_` can hold one directly instead of
  re-searching on cancel (a `std::vector` would invalidate iterators on erase)

## v2 optimizations (milestone 5)

### Profiling methodology (read this before trusting the numbers below)

First attempt profiled `bench_main` directly at 20M ops and the result was
useless: `perf report` showed `std::sort` of the 20M-entry latency vector
and `ofstream << double` (which goes through glibc's locale-aware float
formatting, `__printf_fp_buffer_1`, `__mpn_divrem`, etc.) eating over 50%
of total runtime between them, plus ~10% in the per-op `clock_gettime`
calls. None of that is the engine, it's the benchmark harness measuring
itself.

Fix: `bench/profile_main.cpp`, a minimal driver with none of that, no
per-op timing, no sort, no result file. It generates the op stream once,
then applies it in a tight loop with a single start/end timestamp around
the whole thing. `perf record -e task-clock -g --call-graph fp` against
*that* gives a clean read. (Side finding: removing the per-op
`clock_gettime` overhead alone bumped measured throughput from
~9.4-10.1M ops/sec to ~14.7M ops/sec, that overhead was real and large
relative to a ~70ns operation. `bench_main`'s numbers are legitimate as a
*latency distribution* measurement, just know the per-sample
instrumentation cost is baked into every value.)

### What's actually slow (profile_main, 20M ops, release build)

Excluding harness code (`generateOps`, the RNG, `main`'s loop overhead,
~21% combined), within the engine itself:

| Cost center | Self time (% of total) |
|---|---|
| `OrderBook::addLimit/rest/cancel/match/addMarket` (own logic) | ~42% |
| `malloc`/`cfree` + allocator internals | ~17% |
| `index_` (`unordered_map`) operator\[\]/erase/insert | ~13.5% |
| `std::map` red-black tree insert/rebalance | ~3.85% |
| `std::list` node unhook | ~0.8% (mostly inlined into the OrderBook functions above) |

This matches the original prediction: **node allocation dominates**,
every resting order costs a `std::list` node allocation, every new price
level costs a `std::map` node allocation, and `cancel`/match-driven
removal costs the matching frees. The `unordered_map` index being ~13.5%
on its own was a bit more than expected, more than the `std::map` tree
operations it was meant to make cheap.

### v2 design: `FastOrderBook`

Same public interface as `OrderBook` (drops into the same typed tests and
differential fuzz test), different internals:

- **Flat array of price levels instead of `std::map`.** `bidLevels_` and
  `askLevels_` are `std::vector<Level>` indexed directly by price
  (`levels[price]`), sized to a fixed `kMaxPrice` band (documented
  limitation below). This kills the red-black tree entirely: finding a
  price level is an array index, O(1), not O(log n), and there's no
  per-level node to allocate.
- **Intrusive doubly linked list instead of `std::list`.** Each resting
  order's prev/next are indices stored alongside the order itself in the
  object pool (below), not a separately-allocated list node. Inserting or
  removing an order from a price level's FIFO queue is just pointer
  (index) updates, no allocation.
- **Object pool instead of per-order heap allocation.** Orders live in a
  preallocated `std::vector<PoolNode>` with a free list threaded through
  it via the same prev/next indices. "Allocating" an order is popping the
  free list head; "freeing" one is pushing it back. No `malloc`/`free` on
  the hot path once the pool has grown to cover the live order count.
- **`bestBid`/`bestAsk` are O(1) cached reads, not array scans.** Updated
  directly on insert when a new order improves the cached best. When the
  cached best level empties out, a scan finds the next occupied level,
  bounded by a live resting-order count check so it never scans a
  genuinely empty book looking for something that isn't there. Worst case
  this scan is O(band width) if occupied levels are far apart, versus
  `std::map`'s guaranteed O(log n); acceptable here given how tightly
  clustered real order flow is, and acceptable for this project's scope.
  A guaranteed worst-case bound would need something like a bitset with a
  hierarchical "next set bit" structure, deliberately not built, it's
  real added complexity for a bound that doesn't matter for this
  workload.

**Known limitation, deliberate:** `FastOrderBook` only supports prices in
`[0, kMaxPrice)` (currently 1,000,000) and throws `std::out_of_range`
outside that. `OrderBook` and `NaiveOrderBook` have no such restriction.
This is a real behavioral difference, not just an implementation detail,
and it's why the differential fuzz test's price generator has to stay
inside that band for the three-way comparison to mean anything (it
already does: ~10,000 ± 20). Trading unbounded price range for O(1)
level lookup is a realistic choice for a single-instrument book with a
known practical tick range, not something you'd accept for an instrument
whose price could be anything.

**`index_` (`unordered_map<OrderId, PoolIndex>`) is kept as-is in v2**,
even though profiling flagged it as a real cost. Replacing it (open
addressing, or a dense array if ids are known to be sequential) is a
legitimate further optimization but is out of scope for this pass, it's
not in the original flat-array/intrusive-list/object-pool plan, and
doing it well deserves its own measurement pass rather than being bundled
in here. Noted as a candidate for a future v3, not pretended away.
