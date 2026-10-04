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
- [x] 7 (stretch). pybind11 bindings + Gymnasium environment

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

## Python bindings and Gymnasium environment (stretch goal)

`python/bindings.cpp` exposes both `OrderBook` and `FastOrderBook` to
Python via pybind11, same method set on both (`add_limit`, `add_market`,
`cancel`, `best_bid`, `best_ask`, `depth_at`), snake_cased for Python
convention. `Trade` is exposed read-only. Exceptions need no custom
translator: pybind11's default mapping turns `std::invalid_argument` into
`ValueError` and `std::out_of_range` into `IndexError`, which is exactly
what both engines already throw for bad input.

**Build note:** linking a static library into a Python extension module
(a shared object) requires position-independent code. `orderbook_core`
didn't have that until this stretch goal needed it; `target_properties
... POSITION_INDEPENDENT_CODE ON` fixed a link error
(`relocation ... can not be used when making a shared object`). No
behavioral change, just codegen.

`python/orderbook_gym/env.py` wraps `FastOrderBook` as a Gymnasium
`Env`: a single agent quotes both sides against randomly generated
background order flow (same 60/20/10-ish limit/cancel/market shape as the
C++ generators, reimplemented in Python since the C++ generator isn't
exposed, only the engine is). Reward is the step-over-step change in
mark-to-market net worth (`cash + inventory * mid`), not raw cash flow,
since a market maker holding inventory it paid for isn't "losing" purely
because cash went out, minus a small quadratic inventory penalty.
Verified against `gymnasium.utils.env_checker.check_env`, which passes.

**This is explicitly scaffolding, not a tuned RL problem** (that's "for
later" in the original plan). Known simplifications, deliberate:
- Fixed-length episodes (`max_steps`), no risk-based or inventory-limit
  termination.
- Three-action space (hold / quote both sides / cancel all); no control
  over quote size, asymmetric quoting, or skew based on inventory.
- No adverse-selection modeling; background flow is pure random noise
  around a drifting mid, not informed order flow.
- Reward has no explicit risk-aversion term beyond the inventory penalty.

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
  cached best level empties out, `bidOccupied_`/`askOccupied_` (an
  `OccupancyBitmap`, see the follow-up below) find the next occupied
  level in genuine O(1), a small fixed number of word operations
  independent of how far away that level is.

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

### Follow-up: match() allocated a fresh vector every call

A second review finding: `match()` returned `std::vector<Trade>` by
value, built fresh on every call, a heap allocation on the hot path even
though most calls produce zero or one trade. Exactly the kind of cost v2
was supposed to eliminate, just hiding in a different place than the
price-level/order storage.

**Fix:** `OrderBook` and `FastOrderBook` each gained a `tradeBuffer_`
member, cleared (not deallocated) at the start of every `match()` call
and filled via `push_back`. `addLimit`/`addMarket`/`match()` now return
`const std::vector<Trade>&` referencing it, valid until the next call on
that book. Every existing caller already copies the result immediately
(`auto trades = book.addLimit(...)` or an explicit `std::vector<Trade>`
local), so this required no caller changes. `NaiveOrderBook` was left
alone: it's a correctness oracle, never benchmarked, no reason to add
the complexity there.

**Verified with `perf`**, profiling `FastOrderBook` directly for the
first time (`profile_main.cpp`'s `BENCH_ENGINE=v2`, previously it only
profiled v1): total allocator overhead (`malloc`/`cfree`/`operator new`/
`operator delete` and glibc internals) is now about 5% of engine time,
down from the double digits the original v1 profiling found before any
of v2's allocation-removal work. What's left is overwhelmingly `index_`'s
`unordered_map`, not match()'s old vector: `_M_erase` + `operator[]` +
`_M_insert_unique_node` together are about 11.7% of engine time, more
than double the remaining allocator overhead. This is the first
direct, v2-specific confirmation (rather than inference from v1's
numbers) that `index_` is the single biggest remaining optimization
target for a v3.

### Follow-up: throughput measurement was still contaminated

A third review finding: "measure the clock overhead itself and report
it, and compute throughput by timing whole batches rather than summing
per-op samples." Throughput already came from a single start/end wrapped
around the whole measured loop, not a sum of per-op latency samples, but
that loop's body still contained the per-op `Clock::now()` calls used to
build the latency distribution, so the "whole batch" timer was itself
measuring time spent in clock calls, not purely engine work.

**Fix:** throughput and latency are now measured in two separate passes
against two fresh instances of the same book, replaying the identical
pre-generated op stream. The throughput pass has zero per-op
instrumentation; the latency pass keeps the per-op timestamps, and its
own wall-clock time is never used for a throughput number. Clock-call
overhead is measured once per run (calling `Clock::now()` back to back
200,000 times) and printed alongside the results.

**The effect was large:** v1 throughput went from ~10.05M to ~16.69M
ops/sec (+66%), v2 from ~11.34M to ~20.44M ops/sec (+75%), once the
per-op clock calls were out of the timed region. Clock overhead measured
at ~16-24ns per call, ~33-47ns per latency sample; at v2's ~58ns p50,
over half of that was clock overhead. The v1-vs-v2 *ratio* changed too
(throughput improvement from ~13% to ~22%, p50 from ~24% to ~19%), since
removing a roughly-constant contamination source from both measurements
doesn't preserve their relative comparison. Full before/after numbers in
`bench/RESULTS.md`, which keeps the original contaminated numbers
labeled as superseded rather than deleting them, an honest record over a
retroactively cleaned-up one.

### Follow-up: v2 was O(price range) on a sparse book, not O(1)

An external review of this project caught a real bug in the original
v2 design above: the "bounded by a live-count check so it never scans a
genuinely empty book" claim was true, but didn't mean what it needed to
mean. When the cached best price empties out and there *is* another
occupied level, the old `findNextOccupied` found it with a **linear scan
over the price array**, one array slot at a time, from the old best
toward the new one. That's O(gap between occupied prices), not O(1), and
the original "acceptable given how tightly clustered real order flow is"
justification was an assumption that was never actually tested.

**Reproduction:** one resting bid at price 10, then repeatedly add and
cancel a bid at price 900,000 (`bench/sparse_bench_main.cpp`):

| | ns per add+cancel |
|---|---|
| v1 (`std::map`) | ~46-50 |
| v2, before this fix | ~388,000-432,000 (about 8,000-8,500x slower than v1) |

Every add+cancel at the far price walked roughly 900,000 array slots
looking for the resting order at price 10 (or back again), because that
was the only other occupied level. The main benchmark and the main fuzz
test never caught this: both only ever generate prices in a narrow band
(~±20 ticks around a midpoint), so the next occupied level was always
close by. A sparse book, exactly the kind of input an interviewer asks
"what happens if..." about, broke the O(1) claim completely.

**Fix:** `OccupancyBitmap` (`include/orderbook/occupancy_bitmap.hpp`,
`src/occupancy_bitmap.cpp`), a hierarchical bitmap: one bit per price,
plus a chain of summary levels where level L+1 has one bit per 64-bit
word of level L, set iff that word is non-zero. For `kMaxPrice` =
1,000,000 that's 4 levels (15625, 245, 4, 1 words). Finding the next or
previous occupied price is then at most one O(1) word operation
(`std::countr_zero`/`std::countl_zero`) per level, so at most 4 word
operations total, regardless of how far away the next occupied price is.
`FastOrderBook` maintains one `OccupancyBitmap` per side, setting a bit
when a price level gains its first resting order and clearing it when a
level empties out completely.

**After the fix, same reproduction:** v2 measured ~41-46 ns per
add+cancel, on par with or faster than v1, not 8,000x slower.

**Verification beyond the one repro case:**
- `tests/occupancy_bitmap_test.cpp`: unit tests for the bitmap itself,
  including cases that specifically cross level-1, level-2, and
  level-3 summary boundaries, the exact kind of boundary a subtly wrong
  implementation would get wrong.
- `tests/sparse_fuzz_test.cpp`: a second differential fuzz test (naive
  vs v1 vs v2) using two price clusters ~999,000 ticks apart instead of
  fuzz_test.cpp's single narrow band, so cross-cluster best-price
  transitions get exercised on every run, not just in the one-off
  benchmark repro. Passes at the default 5,000 ops; registered in CTest
  so it runs on every `ctest` invocation, not just on demand.
- `bench/sparse_bench_main.cpp` is now a permanent benchmark (not just a
  one-off repro script), so this specific pathology regressing would show
  up immediately.

**Lesson, worth stating plainly:** a benchmark and a fuzz test only tell
you about the inputs they generate. Both of this project's existing ones
used the same narrow, clustered price distribution, which is realistic
for *typical* order flow but hid a real worst-case bug. "What happens on
a sparse book" is exactly the kind of question that distribution could
never have surfaced on its own.

## Order modification: reduceQty and replacePrice

A review suggested this as a natural feature with a good interview
talking point: the priority rule. Implemented identically (same method
names, same semantics) across all three engines, so they stay
differentially testable the same way everything else in this project is.

- **`reduceQty(id, newQty)`**: shrinks a resting order's quantity in
  place, keeping its FIFO priority. It's still the same order, just
  asking for less, there's no reason to send it to the back of the
  queue. Throws `std::invalid_argument` if `newQty` is 0 or >= the
  order's current quantity (increasing size, or a no-op "reduction",
  isn't what this operation is for). Returns `false` if `id` is unknown.
- **`replacePrice(id, newPrice)`**: cancels the resting order at its old
  price and re-inserts it at `newPrice` with a fresh sequence number,
  losing FIFO priority. It's a different price level; there's no
  queue position to keep. Quantity carries over unchanged. Can match
  immediately if the new price crosses the book. Throws
  `std::invalid_argument` if `id` is unknown (a deliberate choice, see
  below), or `std::out_of_range` (`FastOrderBook` only) if `newPrice` is
  outside `[0, kMaxPrice)`.

**Design decision: `replacePrice` throws on unknown id, `cancel` returns
`false`.** In a real system, trying to amend an order that already got
filled or cancelled is a normal race condition, not a bug, an argument
for returning a sentinel like `cancel` does. This project throws instead,
for API simplicity (one return type, no ambiguity between "not found"
and "found but nothing happened"). That's a deliberate simplification,
not an oversight, and a real production system would need to handle the
race-condition case explicitly.

**`FastOrderBook` implementation note:** `replacePrice` reuses the same
object-pool slot across the price change instead of freeing it and
allocating a new one, via two new shared helpers:
`removeFromBook(order, idx)` (unlink from the old level, update the
occupancy bitmap and cached best price, used by `cancel`, `replacePrice`,
and `match`'s fully-filled-counterparty case) and `placeAtIndex(idx, order)`
(install into a new level, used by `rest` and `replacePrice`). This also
simplified `allocateNode`, which no longer takes an `Order` parameter,
it just reserves a slot; populating it is `placeAtIndex`'s job now,
not duplicated in two places.

**Testing:** typed unit tests cover both the priority-kept and
priority-lost behavior directly (`tests/book_test.cpp`,
`ReduceQtyKeepsFifoPriority` / `ReplacePriceLosesFifoPriority`), plus
error cases. Both fuzz tests (`tests/fuzz_test.cpp`,
`tests/sparse_fuzz_test.cpp`) now generate `reduceQty`/`replacePrice`
ops too (15% each of the op mix), comparing naive/v1/v2 both on thrown
exceptions (consistent throw-or-not across all three) and on resulting
trades/state when none throw. `sparse_fuzz_test.cpp`'s `replacePrice`
reuses its cluster-picking logic, so a meaningful fraction of replaces
jump an order from one price cluster to the other, exactly the
cross-cluster transition that test exists to stress.

Exposed to Python too (`python/bindings.cpp`: `reduce_qty`,
`replace_price`), same exception-translation behavior as the other
methods (no custom work needed).
