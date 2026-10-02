# Order Book

A single-instrument limit order book and matching engine in C++20, built to
be explained line by line: price-time priority matching, integer-tick
prices, a differential fuzz test against a deliberately naive reference
implementation, and a measured (not guessed) v1 → v2 performance story.

See [docs/DESIGN.md](docs/DESIGN.md) for design decisions and milestone
status.

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

Other presets: `release`, `sanitize` (ASan + UBSan, Linux/GCC or Clang only).

## Layout

- `include/orderbook/`: public headers (`types.hpp`, `order_book.hpp`, `fast_order_book.hpp`, `reference_book.hpp`)
- `src/`: implementation
- `tests/`: GoogleTest unit tests, plus a differential fuzz test
- `bench/`: throughput/latency harness, generates its own synthetic order
  flow internally (see `bench/RESULTS.md` for numbers)
- `tools/`: optional LOBSTER sample-data loader, not yet built
- `docs/`: design notes

## Benchmarks

| | Throughput | p50 | p99 | p99.9 |
|---|---|---|---|---|
| v1 (`std::map`/`std::list`) | ~10.05M ops/sec | ~72.5ns | ~232.5ns | ~353.5ns |
| v2 (flat array, intrusive list, object pool) | ~11.34M ops/sec | ~55.3ns | ~213.5ns | ~327.5ns |

Measured on an i7-13620H under WSL2. v2 is a real but modest win (~13%
throughput, ~24% p50), consistent with what profiling v1 found: node
allocation was the biggest cost, v2 removes most of it, but a remaining
`unordered_map` in both engines' order index wasn't touched in this pass.
Full methodology, per-run numbers, and the profiling-to-optimization
story are in [bench/RESULTS.md](bench/RESULTS.md) and
[docs/DESIGN.md](docs/DESIGN.md).

## Status

Milestones 1 through 5 are done: the project is scaffolded, `OrderBook`
(v1) and `FastOrderBook` (v2) both implement price-time priority matching
with the same public interface, a differential fuzz test checks both
against a naive reference book after every operation, and both are
benchmarked (above) with the improvement traced back to what profiling
v1 actually found. See [docs/DESIGN.md](docs/DESIGN.md) for the full
story and what's left (milestone 6: README polish).
