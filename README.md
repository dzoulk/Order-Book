# Order Book

A single-instrument limit order book and matching engine in C++20, built to
be explained line by line: price-time priority matching, integer-tick
prices, a differential fuzz test against a deliberately naive reference
implementation, and a measured (not guessed) v1 → v2 performance story.

## Design decisions

- **Prices are integer ticks (`int64_t`), never floating point.** Binary
  floating point can't represent most decimal fractions exactly, so price
  comparisons and arithmetic on `double` prices would be a source of subtle
  bugs. Ticks (e.g. cents) sidestep that entirely.
- **Time priority uses a monotonic sequence number, not wall-clock
  timestamps.** Clock resolution and monotonicity aren't guaranteed across
  calls; a counter is simpler and deterministic, which matters for the
  differential fuzz test.
- **`NaiveOrderBook`** is a deliberately slow, obviously-correct reference
  implementation (linear scans over a flat vector). It exists purely as a
  fuzz-test oracle, never as a baseline to beat.
- **`OrderBook` (v1):** `std::map<Price, std::list<Order>>` per side
  (ascending for asks, `std::greater` for bids so `begin()` is always the
  best price), plus `std::unordered_map<OrderId, Location>` for cancel.
  `std::list` specifically because erasing one order never invalidates
  iterators to any other order, which is what makes O(1) cancel-by-id
  possible without re-searching. Complexity: `addLimit`/`addMarket`
  O(log P) to find a price level (P = number of distinct price levels) plus
  O(1) per fill; `cancel` O(1); `bestBid`/`bestAsk` O(1).
- **`FastOrderBook` (v2):** profiling v1 showed node allocation
  (`std::map`/`std::list`/allocator internals) was the single biggest cost,
  about 21% of engine time. v2 replaces the price-level map with a flat
  array indexed directly by price, and `std::list` with an intrusive
  doubly-linked list backed by a preallocated object pool, so resting an
  order is a pool-slot grab and pointer updates, no heap allocation.
  Complexity: `addLimit`/`addMarket`/`cancel` O(1) amortized; `bestBid`/
  `bestAsk` O(1) cached read, with a bounded scan only when the cached best
  price level empties out. Trade-off: only supports prices in
  `[0, kMaxPrice)` (see Known limitations).
- Full rationale, the profiling data that drove v2's design, and every
  decision's "why" lives in [docs/DESIGN.md](docs/DESIGN.md).

## Build

Developed in **WSL2 (Ubuntu)**, not native Windows, since `perf` and UBSan
don't run on Windows. The project lives in the Linux filesystem
(`~/order-book`, not `/mnt/c/...`) so builds and benchmarks aren't crossing
the Windows/Linux filesystem boundary. See [docs/DESIGN.md](docs/DESIGN.md)
for why.

Requires CMake 3.21+, [Ninja](https://ninja-build.org/), and a C++20
compiler (`build-essential`/`clang` on Ubuntu).

```
cmake --preset debug
cmake --build --preset debug
ctest --preset debug
```

Other presets: `release` (`-O3`, for benchmarking), `profile` (release plus
debug symbols and frame pointers, for `perf`), `sanitize` (ASan + UBSan,
Linux/GCC or Clang only).

## Testing

`ctest --preset debug` (or `sanitize`) runs everything: the GoogleTest unit
suite (same behavioral tests run against `NaiveOrderBook`, `OrderBook`, and
`FastOrderBook` via typed tests) plus the differential fuzz test.

The fuzz test (`tests/fuzz_test.cpp`) feeds the same random operation stream
to all three books and checks they agree after every single operation, both
on returned trades and on full book state. Configurable via environment
variables:

```
ORDERBOOK_FUZZ_SEED=<seed>   # reproduce a specific run (default: random)
ORDERBOOK_FUZZ_OPS=<n>       # how many ops to run (default: 20000)
```

Default op count is fast enough for routine local runs (about a second).
It's been run up to 5,000,000 ops (v1 only) and 500,000 ops (v1 and v2
together) with zero mismatches, see [docs/DESIGN.md](docs/DESIGN.md) for
why it isn't run at that scale by default (the naive reference's cost
scales roughly quadratically with op count, not linearly).

## Benchmarks

```
cmake --preset release
cmake --build --preset release
./build/release/bench/orderbook_bench
```

Configurable via `BENCH_SEED`, `BENCH_OPS` (default 1,000,000), and
`BENCH_WARMUP_OPS` (default 50,000). Benchmarks v1 and v2 in one run, on
the same pre-generated operation stream, so they're directly comparable.

| | Throughput | p50 | p99 | p99.9 |
|---|---|---|---|---|
| v1 (`std::map`/`std::list`) | ~10.05M ops/sec | ~72.5ns | ~232.5ns | ~353.5ns |
| v2 (flat array, intrusive list, object pool) | ~11.34M ops/sec | ~55.3ns | ~213.5ns | ~327.5ns |

Measured on an Intel Core i7-13620H (8 cores / 16 threads) under WSL2,
`cmake --preset release` (GCC 15.2.0, `-O3 -DNDEBUG`). v2 is a real but
modest win (~13% throughput, ~24% p50), consistent with what profiling v1
found: node allocation was the biggest cost, v2 removes most of it, but a
remaining `unordered_map` in both engines' order index wasn't touched in
this pass. Full per-run numbers and the profiling-to-optimization story are
in [bench/RESULTS.md](bench/RESULTS.md) and [docs/DESIGN.md](docs/DESIGN.md).

## Known limitations

- **`FastOrderBook` only supports prices in `[0, kMaxPrice)`** (currently
  1,000,000) and throws `std::out_of_range` outside that band.
  `OrderBook` and `NaiveOrderBook` have no such restriction. A real
  trade-off for O(1) price-level lookup, not an oversight.
- **The order index (`unordered_map<OrderId, ...>`) wasn't optimized in
  v2**, even though profiling flagged it as a real cost (~13.5% of engine
  time). Replacing it is a legitimate v3 candidate, not in scope here.
- **The differential fuzz test's cost scales roughly quadratically with op
  count**, not linearly, because the naive reference's resting-order count
  grows over a long run and its state check is O(n). Deliberate, since the
  naive book's entire job is being obviously correct, not fast, but it
  means "run it with millions of ops" takes real time, not a quick CI step.
- **Benchmarks run under WSL2, not bare-metal Linux.** Expect some
  overhead and noise versus a dedicated Linux box. Hardware PMU counters
  also aren't available under this WSL2 kernel, so `perf` profiling in
  `docs/DESIGN.md` uses `task-clock` software-event sampling, not
  cycle-accurate hardware counters.
- **Per-op `steady_clock::now()` overhead is baked into every benchmark
  latency sample.** At tens of nanoseconds of actual engine work, the
  clock call itself is a non-trivial fraction of what's measured. Mostly
  affects absolute latency numbers, not the v1-vs-v2 comparison, since the
  same overhead applies to both.
- **Single instrument, no persistence, no network layer.** This is a
  matching-engine library, not a service; there's no order-book recovery,
  multi-symbol routing, or wire protocol, that's out of scope by design.

## Layout

- `include/orderbook/`: public headers (`types.hpp`, `order_book.hpp`, `fast_order_book.hpp`, `reference_book.hpp`)
- `src/`: implementation
- `tests/`: GoogleTest unit tests, plus a differential fuzz test
- `bench/`: throughput/latency harness, generates its own synthetic order
  flow internally (see `bench/RESULTS.md` for numbers)
- `tools/`: optional LOBSTER sample-data loader, not yet built
- `docs/`: design notes

## Status

All six milestones are done: scaffolding, a working v1 (`OrderBook`), a
differential fuzz test and sanitizers, a benchmark harness with v1
results, a profiled-and-optimized v2 (`FastOrderBook`) with a measured
v1-vs-v2 comparison, and this README. See [docs/DESIGN.md](docs/DESIGN.md)
for the complete history, including a stretch goal (pybind11 + Gymnasium
environment for a future RL market-making agent) that was never started.
