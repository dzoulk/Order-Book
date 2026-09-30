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
- `tests/`: GoogleTest unit tests (plus a differential fuzz test, milestone 3)
- `bench/`: throughput/latency harness (milestone 4)
- `tools/`: synthetic order-flow generator / LOBSTER loader (milestone 4)
- `docs/`: design notes

## Status

Milestones 1 and 2 are done: the project is scaffolded and `OrderBook` has
a working v1 implementation with price-time priority matching, backed by a
GoogleTest suite that runs against both it and a naive reference book. See
[docs/DESIGN.md](docs/DESIGN.md) for the full plan and what's next.
