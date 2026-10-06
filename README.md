# Order Book

A single-instrument limit order book and matching engine in C++20. Built
v1 with `std::map`/`std::list`, profiled it, and built a faster v2 (flat
array, intrusive list, object pool, a hierarchical bitmap for O(1)
best-price lookup). Verified against real NASDAQ order flow and a
lock-free concurrent gateway/matching pipeline, not just synthetic,
single-threaded benchmarks; see "Issues found and fixed" below for what
broke along the way and how it was caught.

## Results

| | Throughput | p50 | p99 | p99.9 |
|---|---|---|---|---|
| v1 (`std::map`/`std::list`) | ~18.4M ops/sec | ~67ns | ~176ns | ~294ns |
| v2 (flat array, intrusive list, object pool) | ~25.5M ops/sec | ~49ns | ~151ns | ~257ns |

Measured on an Intel Core i7-13620H under WSL2, `cmake --preset release`
(GCC 15.2.0, `-O3 -DNDEBUG`), realistic op mix (45% limit / 20% cancel /
15% reduceQty / 15% replacePrice / 5% market). Methodology, every
historical measurement, and the full profiling-to-optimization story are
in [bench/RESULTS.md](bench/RESULTS.md) and
[docs/HISTORY.md](docs/HISTORY.md).

## Build

Developed in **WSL2 (Ubuntu)**, not native Windows, since `perf` and UBSan
don't run on Windows. The project lives in the Linux filesystem
(`~/order-book`, not `/mnt/c/...`) so builds and benchmarks aren't crossing
the Windows/Linux filesystem boundary.

Requires CMake 3.21+, [Ninja](https://ninja-build.org/), and a C++20
compiler (`build-essential`/`clang` on Ubuntu).

```
cmake --preset debug
cmake --build --preset debug
ctest --preset debug
```

Other presets: `release` (`-O3`, for benchmarking), `profile` (release plus
debug symbols and frame pointers, for `perf`), `sanitize` (ASan + UBSan,
Linux/GCC or Clang only), `python` (pybind11 bindings).

## Testing

`ctest --preset debug` (or `sanitize`) runs everything: the GoogleTest unit
suite (same behavioral tests run against `NaiveOrderBook`, `OrderBook`, and
`FastOrderBook` via typed tests, plus `OccupancyBitmap`'s own tests), and
two differential fuzz tests.

`tests/fuzz_test.cpp` feeds the same random operation stream
(limit/cancel/reduceQty/replacePrice/market) to all three books and
checks they agree after every single operation, both on returned trades
and on full book state, including consistent throw-or-not behavior for
reduceQty/replacePrice.

```
ORDERBOOK_FUZZ_SEED=<seed>   # reproduce a specific run (default: random)
ORDERBOOK_FUZZ_OPS=<n>       # how many ops to run (default: 20000)
```

Default op count is fast enough for routine local runs (about a second).
Verified at 5,000,000 ops (v1 only) and 500,000 ops (all three engines)
with zero mismatches; see [docs/HISTORY.md](docs/HISTORY.md) for why it
isn't run at that scale by default (the naive reference's cost scales
roughly quadratically with op count).

`tests/sparse_fuzz_test.cpp` runs the same three-way comparison with two
price clusters ~999,000 ticks apart instead of one narrow band, the shape
of book that exposed the sparse-book bug below.
`ORDERBOOK_SPARSE_FUZZ_SEED`/`ORDERBOOK_SPARSE_FUZZ_OPS` configure it the
same way.

## Benchmarks

```
cmake --preset release
cmake --build --preset release
./build/release/bench/orderbook_bench
```

Configurable via `BENCH_SEED`, `BENCH_OPS` (default 1,000,000), and
`BENCH_WARMUP_OPS` (default 50,000). Benchmarks v1 and v2 in one run on
the same pre-generated operation stream. Throughput and per-op latency
are measured in separate passes (see Known limitations); the tool also
measures and prints its own clock-call overhead rather than leaving it
unstated. See the Results table above, and
[bench/RESULTS.md](bench/RESULTS.md) for full methodology and every
historical number.

`./build/release/bench/orderbook_sparse_bench` is a dedicated regression
benchmark for the sparse-book scenario below.

## Real market data replay

`tools/orderbook_itch_replay` replays real NASDAQ ITCH 5.0 order flow (not
synthetic) through both engines, as a check against the risk that a
hand-written generator's op mix happens to flatter one engine. On a
partial-day sample of real AAPL flow (170,839 ops):

| | Throughput |
|---|---|
| v1 (`std::map`/`std::list`) | ~15M ops/sec |
| v2 (flat array, intrusive list, object pool) | ~34M ops/sec |

v2's advantage is bigger here (~2.3x) than on the synthetic benchmark
above (~1.4x). Likely why: this real sample touches 5,517 distinct
price levels versus the synthetic generator's 273, and v1's `std::map`
lookup costs more as distinct price levels grow (O(log P)) while v2's
flat-array-plus-bitmap design doesn't (O(1) regardless of P). See
[docs/HISTORY.md](docs/HISTORY.md) for the full comparison, how ITCH's
one-sided feed maps onto this engine's two-sided API, and
[tools/README.md](tools/README.md) for how to get sample data and run
it yourself.

## Concurrency: gateway thread to matching thread

`bench/orderbook_concurrency_bench` puts a lock-free SPSC ring buffer
(`include/orderbook/spsc_queue.hpp`) between a gateway thread and a
matching thread owning `FastOrderBook`, instead of calling the book
directly in-process, and measures end-to-end latency across that
handoff. Verified race-free under ThreadSanitizer. Measured on the same
machine as the Results table above, same op stream, same process,
pinned to two distinct physical cores:

Saturated (gateway sends flat-out, no delay between messages), p50
scales almost exactly linearly with queue depth, a bounded queue's
Little's Law behavior under sustained overload, not a fixed handoff
cost:

| | Throughput | p50 |
|---|---|---|
| single-threaded, in-process | ~26M ops/sec | ~49ns |
| pipelined, shallow queue (64) | ~8M ops/sec | ~7µs |
| pipelined, deep queue (4096) | ~8M ops/sec | ~440µs |
| pipelined, deep queue, batched (64) | ~9-10M ops/sec | ~330-370µs |

Paced at a fixed offered load well under capacity instead, isolating
the true cross-core handoff cost from queueing delay:

| Offered load | p50 |
|---|---|
| 1,000,000/sec | ~260-280ns |
| 4,000,000/sec | ~260-375ns |
| 7,000,000/sec (exceeds capacity) | ~440-640µs |

The true handoff cost is a few hundred nanoseconds, not microseconds;
the saturated numbers above measure queueing delay once offered load
reaches the matching thread's real capacity (around 6-10M ops/sec
here), not handoff cost. The batch API (`tryPushBatch`/`tryPopBatch`,
one atomic store per batch instead of per message) recovers a real
chunk of the saturated throughput gap, confirming per-message
cross-core traffic as a genuine contributor, though not the only one.
See [docs/HISTORY.md](docs/HISTORY.md) for the full story, including a
hyperthread-sibling pinning mistake that made the saturated numbers 6x
worse before it was caught.

## Python bindings and RL environment (stretch goal)

```
cmake --preset python
cmake --build --preset python
PYTHONPATH=build/python/python .venv/bin/python3 -m pytest python/tests -v
```

(First time: `python3 -m venv .venv && .venv/bin/pip install pybind11 gymnasium numpy pytest`.)

`python/bindings.cpp` exposes both engines to Python via pybind11.
`python/orderbook_gym/env.py` wraps `FastOrderBook` as a Gymnasium `Env`,
scaffolding for a future market-making RL agent, verified against
Gymnasium's own `check_env`. Deliberately not a tuned RL problem; see
[docs/HISTORY.md](docs/HISTORY.md) for what's simplified and why.

## Issues found and fixed

Four real issues surfaced during development, three found through this
project's own profiling and benchmark-methodology scrutiny, one (the
sparse-book case below) caught by an external code review and
reproduced here. Each is fixed, verified with measured before/after
numbers, and has permanent regression coverage so it can't quietly come
back. Full writeups in [docs/HISTORY.md](docs/HISTORY.md).

- **v2 was O(price range) on a sparse book, not O(1).** `bestBid`/
  `bestAsk` finding the next occupied price level used to be a linear
  array scan. A book with one resting order far from the action made it
  ~8,000x slower than v1 (~432,000ns per add+cancel vs v1's ~46ns), since
  the main benchmark and fuzz test both use a narrow, clustered price
  distribution that never exercised this. Fixed with a hierarchical
  bitmap (`OccupancyBitmap`), genuine O(1) regardless of sparsity: now
  measures ~41-46ns on the same scenario, faster than v1 on
  independent re-verification. Permanent coverage:
  `tests/sparse_fuzz_test.cpp`, `bench/sparse_bench_main.cpp`.
- **`match()` allocated a fresh `std::vector<Trade>` every call**, a heap
  allocation on the hot path even when most calls produce zero or one
  trade. Fixed with a reused internal buffer; both engines' allocator
  overhead dropped accordingly (`perf`-verified).
- **Benchmark throughput was contaminated by clock-call overhead** baked
  into the same timed region as the latency samples. Fixed by measuring
  throughput and latency in separate passes; the corrected numbers were
  66-75% higher than the original ones, and the v1-vs-v2 *ratio* shifted
  too.
- **The order index (`index_`) was `std::unordered_map`**, profiling's
  clearest remaining cost in both engines. Switched to
  `ankerl::unordered_dense::map` (contiguous storage, drop-in
  replacement); cut its own cost from ~11-13.5% of engine time to ~3.5%
  in both engines. Measuring this one required an extra detour: the
  first wall-clock comparison looked like a regression, an isolated A/B
  test proved it wasn't the code (session-level thermal throttling after
  many hours of continuous builds was the real cause), so the final
  numbers come from `perf` percentages instead.

## Known limitations

- **`FastOrderBook` only supports prices in `[0, kMaxPrice)`** (currently
  1,000,000) and throws `std::out_of_range` outside that band.
  `OrderBook` and `NaiveOrderBook` have no such restriction. A real
  trade-off for O(1) price-level lookup, not an oversight.
- **The differential fuzz test's cost scales roughly quadratically with op
  count**, not linearly, because the naive reference's resting-order count
  grows over a long run and its state check is O(n). Deliberate, since the
  naive book's entire job is being obviously correct, not fast, but it
  means "run it with millions of ops" takes real time, not a quick CI step.
- **Benchmarks run under WSL2, not bare-metal Linux.** Expect some
  overhead and noise versus a dedicated Linux box. Hardware PMU counters
  also aren't available under this WSL2 kernel, so `perf` profiling uses
  `task-clock` software-event sampling, not cycle-accurate hardware
  counters.
- **Per-op `steady_clock::now()` overhead is baked into every latency
  sample** (p50/p99/p99.9; it does not affect throughput, measured in a
  separate, uninstrumented pass). The benchmark measures and reports this
  overhead directly (~16-24ns per call on this machine) rather than
  leaving it unstated.
- **`index_` (`unordered_dense::map<OrderId, ...>`) is still the largest
  remaining cost center** (~3.5% of engine time in both engines, more
  than double the remaining allocator overhead). A further replacement
  (open addressing tuned for this exact access pattern, or a dense array
  if ids are known to be sequential) is a legitimate v3 candidate, not
  pursued here.
- **Single instrument, no persistence, no network layer.** This is a
  matching-engine library, not a service; there's no order-book recovery,
  multi-symbol routing, or wire protocol, that's out of scope by design.

## Layout

- `include/orderbook/`: public headers (`types.hpp`, `order_book.hpp`, `fast_order_book.hpp`, `occupancy_bitmap.hpp`, `reference_book.hpp`, `spsc_queue.hpp`)
- `src/`: implementation
- `tests/`: GoogleTest unit tests, plus two differential fuzz tests (narrow-band and sparse)
- `bench/`: throughput/latency harness, generates its own synthetic order
  flow internally (see `bench/RESULTS.md` for numbers), a dedicated
  sparse-book regression benchmark, and the gateway/matching concurrency
  benchmark
- `tools/`: `orderbook_itch_replay`, replays real NASDAQ ITCH 5.0 order
  flow through both engines (see [tools/README.md](tools/README.md))
- `docs/`: [DESIGN.md](docs/DESIGN.md) (current design) and
  [HISTORY.md](docs/HISTORY.md) (how it got here, issues found and
  fixed, every historical measurement)
- `python/`: pybind11 bindings (`bindings.cpp`) and a Gymnasium
  environment (`orderbook_gym/`), stretch goal

## Status

All planned milestones and the stretch goal are done: a working v1
(`OrderBook`), a profiled-and-optimized v2 (`FastOrderBook`), differential
fuzz tests and sanitizers, a benchmark harness, real market data replay,
a lock-free concurrency benchmark, Python bindings with a Gymnasium
environment, and a round of correctness and performance fixes (above).
See [docs/DESIGN.md](docs/DESIGN.md) for the system as it stands today,
or [docs/HISTORY.md](docs/HISTORY.md) for the complete story.
