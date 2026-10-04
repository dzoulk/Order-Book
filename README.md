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
  `bestAsk` O(1), including finding the next occupied price level when the
  cached best empties out, via a hierarchical bitmap (`OccupancyBitmap`).
  **That O(1) claim used to be wrong:** an earlier version found the next
  occupied level with a linear scan, genuinely O(price range) on a sparse
  book, see Known limitations for the numbers and the fix. Trade-off:
  only supports prices in `[0, kMaxPrice)` (see Known limitations).
- **Order modification (`reduceQty`, `replacePrice`), same across all
  three engines.** Reducing quantity keeps FIFO priority, it's still the
  same order asking for less. Replacing price loses it, it's a
  different price level with no queue position to preserve. This is the
  real-world priority rule (seen in e.g. FIX Cancel/Replace) made
  explicit as two operations instead of one ambiguous "modify".
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
`FastOrderBook` via typed tests, plus `OccupancyBitmap`'s own tests), the
main differential fuzz test, and a second sparse-book fuzz test.

The main fuzz test (`tests/fuzz_test.cpp`) feeds the same random operation
stream (limit/cancel/reduceQty/replacePrice/market) to all three books
and checks they agree after every single operation, both on returned
trades and on full book state, including consistent throw-or-not
behavior for reduceQty/replacePrice. Configurable via environment
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

`tests/sparse_fuzz_test.cpp` runs the same three-way comparison but with
two price clusters ~999,000 ticks apart instead of one narrow band, the
shape of book that exposed a real O(price range) bug in v2 (see Known
limitations). `ORDERBOOK_SPARSE_FUZZ_SEED`/`ORDERBOOK_SPARSE_FUZZ_OPS`
configure it the same way.

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
| v1 (`std::map`/`std::list`) | ~16.69M ops/sec | ~71.5ns | ~210.5ns | ~333.8ns |
| v2 (flat array, intrusive list, object pool) | ~20.44M ops/sec | ~57.75ns | ~201ns | ~318ns |

Measured on an Intel Core i7-13620H (8 cores / 16 threads) under WSL2,
`cmake --preset release` (GCC 15.2.0, `-O3 -DNDEBUG`). Throughput and
latency are measured in separate passes (see Known limitations), and the
tool reports its own clock-call overhead (~16-24ns per call) alongside
every run rather than leaving it unstated. v2 is a real but modest win
(~22% throughput, ~19% p50), consistent with what profiling v1 found:
node allocation was the biggest cost, v2 removes most of it, but a
remaining `unordered_map` in both engines' order index wasn't touched in
this pass. Full per-run numbers, the earlier (since-corrected)
measurement, and the profiling-to-optimization story are in
[bench/RESULTS.md](bench/RESULTS.md) and [docs/DESIGN.md](docs/DESIGN.md).

That table uses the same narrow, clustered price distribution as the fuzz
test, which turned out to hide a real bug: `./build/release/bench/orderbook_sparse_bench`
benchmarks a deliberately sparse scenario (one resting order far from
where the action is), where v2 used to be about 8,000x slower than v1
before a fix. See Known limitations and [docs/DESIGN.md](docs/DESIGN.md)
for the full story.

## Python bindings and RL environment (stretch goal)

```
cmake --preset python
cmake --build --preset python
PYTHONPATH=build/python/python .venv/bin/python3 -m pytest python/tests -v
```

(First time: `python3 -m venv .venv && .venv/bin/pip install pybind11 gymnasium numpy pytest`.)

`python/bindings.cpp` exposes both `OrderBook` and `FastOrderBook` to
Python via pybind11. `python/orderbook_gym/env.py` wraps `FastOrderBook`
as a Gymnasium `Env`, scaffolding for a future market-making RL agent,
verified against Gymnasium's own `check_env`. This is deliberately not a
tuned RL problem, fixed-length episodes, a 3-action space, no
adverse-selection modeling, see [docs/DESIGN.md](docs/DESIGN.md) for the
full list of what's simplified and why.

## Known limitations

- **Found and fixed: v2 was O(price range) on a sparse book, not O(1).**
  An external review caught this. `bestBid`/`bestAsk` finding the next
  occupied level used to be a linear array scan; a book with one resting
  order far from the action made it ~8,000x slower than v1 (measured:
  ~432,000ns per add+cancel vs v1's ~46ns). The main benchmark and fuzz
  test both use a narrow, clustered price distribution and never
  exercised this. Fixed with a hierarchical bitmap (`OccupancyBitmap`),
  genuine O(1) regardless of how sparse the book is; now v2 measures
  ~41-46ns on the exact same scenario, on par with v1. Permanent
  regression coverage: `bench/sparse_bench_main.cpp` and
  `tests/sparse_fuzz_test.cpp`. Full writeup in
  [docs/DESIGN.md](docs/DESIGN.md).
- **`FastOrderBook` only supports prices in `[0, kMaxPrice)`** (currently
  1,000,000) and throws `std::out_of_range` outside that band.
  `OrderBook` and `NaiveOrderBook` have no such restriction. A real
  trade-off for O(1) price-level lookup, not an oversight.
- **The order index (`unordered_map<OrderId, ...>`) wasn't optimized in
  v2**, even though profiling flagged it as a real cost. `match()` used
  to also allocate a fresh `std::vector<Trade>` every call (fixed, both
  engines now reuse an internal buffer); with that gone, profiling
  `FastOrderBook` directly shows `index_` at ~11.7% of engine time, more
  than double the remaining allocator overhead (~5%). The clearest v3
  candidate, not in scope here.
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
- **Per-op `steady_clock::now()` overhead is baked into every latency
  sample** (p50/p99/p99.9; it does not affect throughput, which is now
  measured in a separate, uninstrumented pass, see `docs/DESIGN.md` for
  why that distinction turned out to matter a lot). At tens of
  nanoseconds of actual engine work, the clock call itself is a
  non-trivial fraction of what's measured; the benchmark now measures
  and reports this overhead directly (~16-24ns per call on this
  machine) rather than leaving it unstated.
- **Single instrument, no persistence, no network layer.** This is a
  matching-engine library, not a service; there's no order-book recovery,
  multi-symbol routing, or wire protocol, that's out of scope by design.

## Layout

- `include/orderbook/`: public headers (`types.hpp`, `order_book.hpp`, `fast_order_book.hpp`, `occupancy_bitmap.hpp`, `reference_book.hpp`)
- `src/`: implementation
- `tests/`: GoogleTest unit tests, plus two differential fuzz tests (narrow-band and sparse)
- `bench/`: throughput/latency harness, generates its own synthetic order
  flow internally (see `bench/RESULTS.md` for numbers), plus a dedicated
  sparse-book regression benchmark
- `tools/`: optional LOBSTER sample-data loader, not yet built
- `docs/`: design notes
- `python/`: pybind11 bindings (`bindings.cpp`) and a Gymnasium
  environment (`orderbook_gym/`), stretch goal

## Status

All six planned milestones are done, plus the optional stretch goal:
scaffolding, a working v1 (`OrderBook`), a differential fuzz test and
sanitizers, a benchmark harness with v1 results, a profiled-and-optimized
v2 (`FastOrderBook`) with a measured v1-vs-v2 comparison, this README,
and Python bindings with a Gymnasium environment for a future RL
market-making agent. See [docs/DESIGN.md](docs/DESIGN.md) for the
complete history.
