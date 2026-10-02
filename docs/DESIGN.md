# Design Notes

Tracks design decisions and rationale as the project progresses. Sections
get filled in as the milestone they belong to is completed.

## Milestones

- [x] 1. Scaffolding, CMake, CI, empty `OrderBook` interface, reference book
- [x] 2. v1 engine + unit tests passing
- [x] 3. Fuzz harness + sanitizers green
- [x] 4. Benchmark harness + v1 results
- [ ] 5. Profile, v2 optimizations, v1 vs v2 results
- [ ] 6. README polish

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
Verified at 5,000,000 ops on the release build with zero mismatches.

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

TODO, to be filled in after profiling v1 with `perf`.
