# Design

The system as it stands today. For how it got here, bugs found and
fixed, and every historical measurement, see [HISTORY.md](HISTORY.md).

## Core decisions

- **Prices are integer ticks (`Price = int64_t`), never floating point.**
  Avoids binary-fraction rounding errors in price comparisons and
  arithmetic.
- **Time priority uses a monotonic sequence number (`SeqNum`), not
  wall-clock timestamps.** Clock resolution/monotonicity isn't
  guaranteed across calls; a counter is simpler and deterministic for
  the differential fuzz test.
- **Invalid input (`qty == 0`, duplicate id) throws
  `std::invalid_argument`.** Programmer errors, distinct from `cancel`'s
  expected-to-sometimes-fail `bool` return for an unknown id.
- **"Duplicate id" means an order with that id is currently resting.**
  Once an id has fully filled or been cancelled, it can be reused.
- **`NaiveOrderBook`** intentionally uses linear scans over a flat
  `std::vector<Order>`. It exists to be obviously correct, not fast; the
  fuzz tests diff the real engines against it.
- **Sanitizers (ASan/UBSan) run in CI on Linux only.** UBSan isn't well
  supported on MSVC; the Windows CI job is a plain build+test smoke
  check.
- **Local development happens in WSL2 (Ubuntu), not native Windows.**
  `perf` doesn't run on Windows, UBSan isn't supported on MSVC, ASan is
  limited there too. The project lives at `~/order-book` inside the
  WSL2 Linux filesystem (not `/mnt/c/...`) since builds and benchmarks
  across that boundary are dramatically slower and would distort
  measurements.

## `OrderBook` (v1)

- `bids_`: `std::map<Price, std::list<Order>, std::greater<Price>>`
- `asks_`: `std::map<Price, std::list<Order>>`
- `index_`: `ankerl::unordered_dense::map<OrderId, Location>` for O(1)
  cancel, where `Location` holds the side, price, and a
  `std::list<Order>::iterator`
- `std::list` iterators stay valid when other elements are erased or
  inserted, which is why `index_` can hold one directly instead of
  re-searching on cancel (a `std::vector` would invalidate iterators on
  erase)
- Complexity: `addLimit`/`addMarket` O(log P) to find a price level (P =
  distinct price levels) plus O(1) per fill; `cancel`/`reduceQty` O(1);
  `bestBid`/`bestAsk` O(1)

## `FastOrderBook` (v2)

Same public interface as `OrderBook`, built for speed instead of
simplicity:

- **Flat array of price levels instead of `std::map`.** `bidLevels_`/
  `askLevels_` are `std::vector<Level>` indexed directly by price,
  sized to a fixed `kMaxPrice` band (see Known limitations). O(1) array
  indexing, no per-level node to allocate.
- **Intrusive doubly linked list instead of `std::list`.** Each resting
  order's prev/next are indices stored alongside the order itself in the
  object pool, not a separately-allocated list node.
- **Object pool instead of per-order heap allocation.** Orders live in a
  preallocated `std::vector<PoolNode>` with a free list threaded through
  it. No `malloc`/`free` on the hot path once the pool covers the live
  order count.
- **`index_`: `ankerl::unordered_dense::map<OrderId, PoolIndex>`**, same
  rationale as v1's.
- **`bestBid`/`bestAsk` are O(1) cached reads**, including finding the
  next occupied price level when the cached best empties out, via
  `bidOccupied_`/`askOccupied_` (`OccupancyBitmap`, a hierarchical
  bitmap: one bit per price plus a chain of summary levels, at most 4
  word operations regardless of how far away the next occupied price
  is, see HISTORY.md for why this exists).
- Complexity: `addLimit`/`addMarket`/`cancel`/`reduceQty` O(1)
  amortized; `bestBid`/`bestAsk` O(1).

**Known limitation, deliberate:** only prices in `[0, kMaxPrice)`
(currently 1,000,000) are supported; `addLimit`/`replacePrice` throw
`std::out_of_range` outside that. `OrderBook` and `NaiveOrderBook` have
no such restriction. Trading unbounded price range for O(1) level lookup
is realistic for a single-instrument book with a known practical tick
range.

## Order modification: reduceQty and replacePrice

- **`reduceQty(id, newQty)`**: shrinks a resting order's quantity in
  place, keeping FIFO priority (still the same order, asking for less).
  Throws `std::invalid_argument` if `newQty` is 0 or >= current quantity.
  Returns `false` if `id` is unknown.
- **`replacePrice(id, newPrice)`**: cancels the resting order at its old
  price and re-inserts it at `newPrice` with a fresh sequence number,
  losing FIFO priority (a different price level has no queue position
  to keep). Can match immediately if the new price crosses the book.
  Throws `std::invalid_argument` if `id` is unknown (see below), or
  `std::out_of_range` (`FastOrderBook` only) if `newPrice` is outside
  `[0, kMaxPrice)`.

**`replacePrice` throws on unknown id; `cancel` returns `false`.** A real
system treats amending an already-filled/cancelled order as a normal
race condition, an argument for a sentinel return like `cancel`'s. This
project throws instead, for API simplicity, one return type, no
ambiguity between "not found" and "found but nothing happened". A
deliberate simplification: a production system would need to handle
the race condition explicitly.

## Real market data replay

`tools/orderbook_itch_replay` parses NASDAQ ITCH 5.0 (`tools/itch/`) and
replays one symbol's real order flow for a day through both engines.
ITCH's Executed/Cancel messages map to `reduceQty`/`cancel` rather than a
synthesized counterparty order (ITCH only publishes matching's outcome,
never a replayable aggressive order); a fixed price band filters the
small fraction of real orders placed at extreme marketable-limit prices,
the same idea as a real exchange's price-collar gateway check. See
[tools/README.md](../tools/README.md) for usage and HISTORY.md for the
full design rationale.

## Toolchain

WSL2 Ubuntu 26.04: `build-essential clang cmake ninja-build gdb
linux-tools-generic`. Presets: `debug`, `release`, `profile` (release +
debug symbols + frame pointers, for `perf`), `sanitize` (ASan+UBSan,
Linux/GCC/Clang only), `python` (pybind11 bindings, requires `.venv`).

**`perf` caveat:** hardware PMU counters aren't available under this
WSL2 kernel; `perf stat -e cycles` fails with "Unable to find PMU".
`task-clock`-based software-event sampling works fine and is what every
profiling session in this project uses.

## Python bindings and Gymnasium environment

`python/bindings.cpp` exposes both engines via pybind11, same method set
on both, snake_cased. `python/orderbook_gym/env.py` wraps `FastOrderBook`
as a Gymnasium `Env` for a future market-making RL agent, deliberately
scaffolding rather than a tuned RL problem (fixed-length episodes, a
3-action space, no adverse-selection modeling). See HISTORY.md for the
full design rationale.
