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

- `include/orderbook/`: public headers (`types.hpp`, `order_book.hpp`, `reference_book.hpp`)
- `src/`: implementation
- `tests/`: GoogleTest unit tests, plus a differential fuzz test
- `bench/`: throughput/latency harness, generates its own synthetic order
  flow internally (see `bench/RESULTS.md` for numbers)
- `tools/`: optional LOBSTER sample-data loader, not yet built
- `docs/`: design notes

## Benchmarks

v1: ~9.4-10.1M ops/sec, p50 ~70-75ns, p99 ~225-240ns, p99.9 ~350-390ns, on
an i7-13620H under WSL2. Full methodology and caveats in
[bench/RESULTS.md](bench/RESULTS.md).

## Status

Milestones 1 through 4 are done: the project is scaffolded, `OrderBook` has
a working v1 implementation with price-time priority matching, a
differential fuzz test checks it against a naive reference book after
every operation (verified at 5,000,000 ops with zero mismatches), and it's
benchmarked (above). See [docs/DESIGN.md](docs/DESIGN.md) for the full plan
and what's next: profiling v1 and optimizing it into v2.
