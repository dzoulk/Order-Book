# Project History

The chronological record of how this project was built: milestones, what
broke, what was measured, and why. For the design as it stands today, see
[DESIGN.md](DESIGN.md). For current benchmark numbers, see
[../bench/RESULTS.md](../bench/RESULTS.md) or the README.

## Milestones

- [x] 1. Scaffolding, CMake, CI, empty `OrderBook` interface, reference book
- [x] 2. v1 engine + unit tests passing
- [x] 3. Fuzz harness + sanitizers green
- [x] 4. Benchmark harness + v1 results
- [x] 5. Profile, v2 optimizations, v1 vs v2 results
- [x] 6. README polish
- [x] 7 (stretch). pybind11 bindings + Gymnasium environment
- [x] 8. External code review: five issues found and fixed (see "Bugs
      found and fixed" below), plus a second round of polish
- [x] 9. Real market data replay: NASDAQ ITCH 5.0 parser, both engines
      benchmarked against real order flow instead of only synthetic

## Toolchain

Set up in WSL2 Ubuntu 26.04: `build-essential clang cmake ninja-build gdb
linux-tools-generic`. All presets (`debug`, `release`, `profile`,
`sanitize`, `python`) configure, build, and pass `ctest` cleanly in this
environment.

**`perf` caveat:** hardware PMU counters (`cycles`, `instructions`,
`cache-misses`) aren't available under the WSL2 kernel. `perf stat -e cycles`
fails with "Unable to find PMU". Software events work fine though
(`perf stat -e task-clock,context-switches,page-faults`, and
`perf record -e task-clock -g` for call-graph sampling), enough to find
which functions are hot, which is the actual goal.

## Fuzz harness, and a timing lesson from the first large run

`tests/fuzz_test.cpp` runs the same random stream of operations against
`NaiveOrderBook`, `OrderBook`, and `FastOrderBook`, checking they agree
after every single op, both on returned trades and on full book state.
Default op count is 20,000 (about a second); it's been run up to
5,000,000 ops (v1 only) and 500,000 ops (all three engines) with zero
mismatches.

**Cost scales roughly quadratically with op count, not linearly.** 500k
ops took 149s (~298µs/op); the 5M-op run took ~4.76ms/op, about 16x
worse at 10x more ops. `NaiveOrderBook`'s `resting_` grows over a long
run, and `depthAt` is O(n), so every per-op state check gets slower as
the run goes on; total cost across a run is roughly O(n²). An acceptable
property of a deliberately-naive oracle, not a bug.

**The 5M-op run also surfaced a real measurement-hygiene lesson:** the
process reported about 6.6 hours of CPU time but almost 27 hours of
wall-clock elapsed time (24% average CPU), and the system clock rolled
over a day during the run. The WSL2 VM was clearly asleep, suspended, or
heavily throttled for most of that stretch rather than actually
computing. The correctness result still held (it's independent of
timing), but wall-clock `time` across a sleep/suspend is meaningless, a
lesson that recurred later in a different form (see "index_ replaced
with unordered_dense" below).

## Benchmark harness, and a profiling-methodology detour

`bench/bench_main.cpp` only benchmarks `OrderBook`/`FastOrderBook`, never
`NaiveOrderBook` (O(n) by design, a correctness oracle, not a baseline).
The op stream is pre-generated before timing starts, so RNG overhead
never lands inside a measured sample.

**First profiling attempt was useless.** Profiling `bench_main` directly
at 20M ops showed `perf report` dominated by `std::sort` of the 20M-entry
latency vector and `ofstream << double` (glibc's locale-aware float
formatting), over 50% of total runtime between them, plus ~10% in the
per-op `clock_gettime` calls. None of that is the engine, it's the
benchmark harness measuring itself.

**Fix:** `bench/profile_main.cpp`, a minimal driver with none of that: no
per-op timing, no sort, no result file, just the op stream applied in a
tight loop with a single start/end timestamp around the whole thing.
Removing the per-op `clock_gettime` overhead alone bumped measured
throughput from ~9.4-10.1M to ~14.7M ops/sec on v1, that overhead was
real and large relative to a ~70ns operation.

## Python bindings and Gymnasium environment (stretch goal)

`python/bindings.cpp` exposes both engines to Python via pybind11, same
method set on both, snake_cased. Exceptions need no custom translator:
pybind11's default mapping turns `std::invalid_argument` into
`ValueError` and `std::out_of_range` into `IndexError`, exactly what both
engines already throw.

**Build note:** linking a static library into a Python extension module
(a shared object) requires position-independent code, which
`orderbook_core` didn't have until this needed it;
`POSITION_INDEPENDENT_CODE ON` fixed a link error. No behavioral change,
just codegen.

`python/orderbook_gym/env.py` wraps `FastOrderBook` as a Gymnasium `Env`:
a single agent quotes both sides against randomly generated background
flow. Reward is the step-over-step change in mark-to-market net worth
(`cash + inventory * mid`), not raw cash flow, since a market maker
holding inventory it paid for isn't "losing" purely because cash went
out. Verified against `gymnasium.utils.env_checker.check_env`. Explicitly
scaffolding, not a tuned RL problem: fixed-length episodes, a 3-action
space, no adverse-selection modeling, no risk-aversion term beyond a
quadratic inventory penalty.

## Real market data replay

A synthetic benchmark, however carefully generated, invites a fair
question: does it just happen to flatter the engine being measured?
`tools/orderbook_itch_replay` answers that with real order flow instead
of an argument. It parses NASDAQ ITCH 5.0, NASDAQ's own binary protocol
for publishing full order-book activity, and replays one trading day's
messages for a chosen symbol through both engines.

**Protocol shape:** a flat stream of `[2-byte big-endian length][message
body]` records (`tools/itch/itch_reader.cpp`), each body a 1-byte message
type followed by big-endian fields. A Stock Directory message ('R') is
emitted once per listed symbol near the start of the day, mapping a
locate code to a ticker; every other message type that matters here
carries a locate code instead of the ticker text, so finding one
symbol's flow is a two-pass job: scan once for the directory, then scan
again filtering by that symbol's locate code.

**Mapping ITCH to engine operations is not 1:1, documented in
`itch_replay_main.cpp`'s header comment rather than glossed over.**
ITCH's TotalView-style feed only publishes the *outcome* of matching: an
Executed message says a resting order's quantity shrank by some amount,
never a replayable incoming order for the other side of that trade.
There is no way to reconstruct a fictitious counterparty order from this
feed alone. 'E'/'C' (Executed) and 'X' (Cancel, partial) map to
`reduceQty` (or `cancel` if the remaining quantity hits zero); 'D'
(Delete) maps to `cancel`; 'A'/'F' (Add) maps to `addLimit`; 'U'
(Replace) maps to `cancel` of the old reference plus `addLimit` of the
new one, rather than this project's own `replacePrice`, because ITCH's
Replace can change both price and quantity and always assigns a new
order reference, which doesn't fit `replacePrice`'s same-id,
price-only contract. This reproduces the real quantity-decrease and
arrival pattern on the resting side, the same price/size/timing
distributions as the real market, without pretending the engine
discovered a trade it has no data to reconstruct.

**First run crashed:** `FastOrderBook::addLimit: price out of supported
band`. A Python pass over the raw file explained why: real AAPL order
flow includes a small fraction of orders at deliberately extreme prices,
up to $199,999.99, effectively marketable limits meant to guarantee a
fill rather than real price discovery, far outside `kMaxPrice`. Real
exchanges reject orders like this at a price-collar gateway check before
they ever reach matching. Percentile analysis of the same sample (p0.1%
= $1, median = $162.18, p99% = $245, p99.9% = $310) picked a $50-$500
band that keeps 99.71% of real orders; `itch_replay_main.cpp` filters
anything outside it the same way a real gateway would, and reports how
many ops it dropped for that reason on every run (278 of 171,128 in the
AAPL sample used for the numbers below).

**Result**, one trading day's AAPL flow (170,839 ops after filtering:
103,592 add, 63,792 cancel, 3,455 reduceQty):

| | Throughput |
|---|---|
| v1 (`std::map`/`std::list`) | ~15M ops/sec |
| v2 (flat array, intrusive list, object pool) | ~34M ops/sec |

The same roughly 2.2x v2 advantage the synthetic benchmark shows, now on
flow this project didn't generate.

## Bugs found and fixed

An external code review of the finished v1/v2/fuzz/benchmark project
caught four real issues, in addition to suggesting the order
modification feature below. Each one follows the same shape: here's what
was wrong, here's the fix, here's the measured before/after.

### 1. match() allocated a fresh vector every call

`match()` returned `std::vector<Trade>` by value, built fresh on every
call, a heap allocation on the hot path even though most calls produce
zero or one trade. Exactly the kind of cost v2 was supposed to eliminate,
just hiding in a different place than the price-level/order storage.

**Fix:** `OrderBook` and `FastOrderBook` each gained a `tradeBuffer_`
member, cleared (not deallocated) at the start of every `match()` call
and filled via `push_back`. `addLimit`/`addMarket`/`match()` now return
`const std::vector<Trade>&` referencing it, valid until the next call on
that book. Every caller already copied the result immediately (`auto
trades = book.addLimit(...)`), so no caller changes were needed.
`NaiveOrderBook` was left alone: it's a correctness oracle, never
benchmarked.

**Verified with `perf`**, profiling `FastOrderBook` directly for the
first time: total allocator overhead dropped to about 5% of engine time,
down from the double digits the original v1 profiling found. What's left
is overwhelmingly `index_`'s hash map, not the old vector: `_M_erase` +
`operator[]` + `_M_insert_unique_node` together are about 11.7% of engine
time, more than double the remaining allocator overhead, the first
direct (not inferred) confirmation that `index_` was the next target.

### 2. Benchmark throughput was still contaminated by clock-call overhead

Throughput already came from a single start/end wrapped around the whole
measured loop, not a sum of per-op latency samples, but that loop's body
still contained the per-op `Clock::now()` calls used for the latency
distribution, so the "whole batch" timer was itself measuring time spent
in clock calls, not purely engine work.

**Fix:** throughput and latency are now measured in two separate passes
against two fresh book instances replaying the identical op stream. The
throughput pass has zero per-op instrumentation; the latency pass keeps
the per-op timestamps, and its own wall-clock time is never used for a
throughput number. Clock-call overhead is measured once per run (calling
`Clock::now()` back to back 200,000 times) and printed alongside the
results instead of staying an unstated caveat.

**The effect was large:** v1 throughput went from ~10.05M to ~16.69M
ops/sec (+66%), v2 from ~11.34M to ~20.44M ops/sec (+75%), once the
per-op clock calls were out of the timed region. Clock overhead measured
at ~16-24ns per call, ~33-47ns per latency sample; at v2's ~58ns p50 at
the time, over half of that was clock overhead, not engine work. The
v1-vs-v2 *ratio* changed too (throughput improvement from ~13% to ~22%,
p50 from ~24% to ~19%), since removing a roughly-constant contamination
source from both measurements doesn't preserve their relative
comparison, worth knowing on its own: a flawed measurement doesn't just
inflate absolute numbers, it can distort the comparison the exercise
exists to make.

### 3. v2 was O(price range) on a sparse book, not O(1)

The original v2 design's claim, that `bestBid`/`bestAsk` were O(1)
"bounded by a live-count check so it never scans a genuinely empty
book", was true but didn't mean what it needed to mean. When the cached
best price emptied out and there *was* another occupied level,
`findNextOccupied` found it with a **linear scan over the price array**,
one slot at a time. That's O(gap between occupied prices), not O(1), and
the "acceptable given how tightly clustered real order flow is"
justification was an assumption that had never actually been tested.

**Reproduction:** one resting bid at price 10, then repeatedly add and
cancel a bid at price 900,000:

| | ns per add+cancel |
|---|---|
| v1 (`std::map`) | ~46-50 |
| v2, before this fix | ~388,000-432,000 (about 8,000-8,500x slower than v1) |

Every add+cancel at the far price walked roughly 900,000 array slots
looking for the only other occupied level. The main benchmark and fuzz
test never caught this: both only generate prices in a narrow band
(~±20 ticks), so the next occupied level was always close by. A sparse
book, exactly the kind of input an interviewer asks "what happens if..."
about, broke the O(1) claim completely.

**Fix:** `OccupancyBitmap` (`include/orderbook/occupancy_bitmap.hpp`), a
hierarchical bitmap: one bit per price, plus a chain of summary levels
where level L+1 has one bit per 64-bit word of level L, set iff that
word is non-zero. For `kMaxPrice` = 1,000,000 that's 4 levels (15625,
245, 4, 1 words). Finding the next or previous occupied price is then at
most one O(1) word operation (`std::countr_zero`/`std::countl_zero`) per
level, at most 4 total, regardless of how far away the next occupied
price is.

**After the fix, same reproduction:** v2 measured ~41-46ns per
add+cancel, on par with or faster than v1, not 8,000x slower.
Independently re-verified later (see `bench/RESULTS.md`): 104ns vs v1's
146ns on the same scenario, v2 now faster than v1 on its own former
worst case.

**Permanent regression coverage**, so this can't quietly come back:
`tests/occupancy_bitmap_test.cpp` (including cases that cross level-1/2/3
summary boundaries), `tests/sparse_fuzz_test.cpp` (a second differential
fuzz test using two price clusters ~999,000 ticks apart, registered in
CTest), and `bench/sparse_bench_main.cpp` (a permanent benchmark, not a
one-off script).

**Lesson, worth stating plainly:** a benchmark and a fuzz test only tell
you about the inputs they generate. Both of this project's existing ones
used the same narrow, clustered price distribution, realistic for
*typical* order flow but hiding a real worst-case bug.

### 4. index_ replaced with unordered_dense

Profiling had flagged `index_` (`std::unordered_map<OrderId, ...>`) as
the single biggest remaining cost in both engines (~13.5% of v1's engine
time originally, ~11.7% of v2's after fix #1 above), bigger than the
remaining allocator overhead in each case. `std::unordered_map` is
node-based: every entry is a separately heap-allocated node, exactly the
kind of cost v2 has been about removing all along.

**Fix:** swapped `index_`'s type to `ankerl::unordered_dense::map` in
both engines (CMake `FetchContent`, pinned to v5.3.0, same pattern as
GoogleTest/pybind11). A true drop-in replacement, same method names, so
the only code change was the type declaration in each header. It stores
entries contiguously instead of as separate nodes.

**Measuring this properly took a detour worth recording.** The first
wall-clock benchmark after the change showed v1 getting *slower*
(~16.69M down to ~15M ops/sec) while v2 stayed roughly flat, backwards
for a change touching both engines' `index_` identically. Before
believing that, an isolated A/B test reverted just `OrderBook`'s
`index_` back to `std::unordered_map` (keeping `FastOrderBook` on
`unordered_dense`) and re-benchmarked: v1 got *even slower*
(~11.6-11.9M ops/sec) with the old map, not faster, the opposite of what
a real regression from `unordered_dense` would show. By this point in
the session there had been many hours of continuous heavy compilation,
sanitizer runs, and benchmarking; sustained thermal throttling on laptop
hardware is the far more likely explanation than a one-line type swap
making an unrelated code path slower.

**What's actually trustworthy instead:** `perf`'s percentage breakdown,
self-normalizing against whatever the CPU's current clock speed happens
to be, unlike an absolute ops/sec number compared against one measured
hours earlier.

| | `index_` cost | Allocator overhead |
|---|---|---|
| v1, before (`std::unordered_map`) | ~13.5% | ~17.3% |
| v1, after (`unordered_dense`) | ~3.25% | ~10.75% |
| v2, before (`std::unordered_map`) | ~11.7% | ~5% |
| v2, after (`unordered_dense`) | ~3.5% | ~0.04% |

v2's allocator overhead is now essentially eliminated. v1's remaining
allocator cost is `std::map`/`std::list` node allocations for
`bids_`/`asks_`, untouched by this change and not in scope.

**Lesson:** wall-clock comparisons taken hours apart in a long, CPU-heavy
session aren't trustworthy on their own, the same measurement-hygiene
mistake as the WSL2 sleep/suspend finding above, just a different
flavor of it (session-level thermal drift instead of a sleeping VM).

**New dependency:** `ankerl::unordered_dense` (MIT, header-only), the
first third-party runtime dependency in this project beyond test/build
tooling.

## Order modification: reduceQty and replacePrice

Added as a natural feature with a good interview talking point: the
priority rule. Implemented identically across all three engines so they
stay differentially testable.

- **`reduceQty(id, newQty)`**: shrinks a resting order's quantity in
  place, keeping FIFO priority (still the same order, just asking for
  less). Throws `std::invalid_argument` if `newQty` is 0 or >= current
  quantity. Returns `false` if `id` is unknown.
- **`replacePrice(id, newPrice)`**: cancels the resting order at its old
  price and re-inserts it at `newPrice` with a fresh sequence number,
  losing FIFO priority (a different price level has no queue position to
  keep). Can match immediately if the new price crosses the book. Throws
  `std::invalid_argument` if `id` is unknown, or `std::out_of_range`
  (`FastOrderBook` only) if `newPrice` is outside `[0, kMaxPrice)`.

**Design decision: `replacePrice` throws on unknown id, `cancel` returns
`false`.** In a real system, amending an order that already got filled
or cancelled is a normal race condition, an argument for a sentinel
return like `cancel`'s. This project throws instead, for API simplicity
(one return type, no ambiguity between "not found" and "found but
nothing happened"), a deliberate simplification, not an oversight; a
production system would need to handle the race condition explicitly.

**`FastOrderBook` implementation note:** `replacePrice` reuses the same
object-pool slot across the price change instead of freeing and
reallocating, via two new shared helpers: `removeFromBook` (unlink from
the old level, update the occupancy bitmap and cached best price) and
`placeAtIndex` (install into a new level). Also simplified
`allocateNode`, which no longer takes an `Order` parameter, it just
reserves a slot now; populating it is `placeAtIndex`'s job.

**Testing:** typed unit tests cover both the priority-kept and
priority-lost behavior directly, plus error cases. Both fuzz tests
generate `reduceQty`/`replacePrice` ops too, comparing all three engines
both on thrown exceptions and on resulting trades/state. Exposed to
Python too.

## Benchmark op mix, and an exception-cost lesson

Adding `reduceQty`/`replacePrice` to the benchmark's generated flow (not
just the fuzz tests) surfaced a second measurement artifact. The first
attempt picked targets from the same recency-biased window `cancel`
already used, without checking they were actually still valid, a
meaningful fraction weren't (something else had consumed the order
first), and replaying a `std::invalid_argument` throw on the hot path
blew up p99.9 by roughly 30x: C++ exception unwinding costs microseconds,
not nanoseconds, not a property of the engine being benchmarked, just of
a sloppy generator.

**Fix:** the generator now drives a real `NaiveOrderBook` instance
alongside building the op list (a new `restingQty(id)` query was added
to support this), so every `reduceQty`/`replacePrice` target and
`reduceQty`'s `newQty` are verified against the order's actual current
state before being emitted. Every op in the generated stream is
guaranteed valid against the implementation already certified correct
by the differential fuzz tests, so the real engines being benchmarked
never throw at all. `profile_main.cpp` opts out of the modify ops
entirely (`includeModifyOps=false`), keeping its profile exactly as
clean as before they existed.
