# Design Notes

Tracks design decisions and rationale as the project progresses. Sections
get filled in as the milestone they belong to is completed.

## Milestones

- [x] 1. Scaffolding, CMake, CI, empty `OrderBook` interface, reference book
- [x] 2. v1 engine + unit tests passing
- [ ] 3. Fuzz harness + sanitizers green
- [ ] 4. Benchmark harness + v1 results
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
