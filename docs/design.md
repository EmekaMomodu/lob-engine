# Design decisions (living document)

This file records decisions as they are made, with the alternatives that were
rejected. The final README (Milestone 7) is built from it. Decisions marked
**provisional** get confirmed or revised by measurement in a later milestone.

## Scope

- Single instrument. Multi-symbol is one engine instance per symbol, each on
  its own pinned core (see "Threading").
- Order types: GTC limit, market (IOC), cancel, modify.
- Integer prices (ticks, `int64`) and quantities. No floating point in the
  matching path (`-Wfloat-equal` is on to catch accidental use).

## Matching semantics

| Case | Behavior |
|---|---|
| Limit that crosses | Fills against the opposite side at the **resting** price; remainder rests (GTC) |
| Market | Sweeps until filled or that side of the book is empty; remainder is cancelled (IOC) |
| Market, empty or partly fillable book | `Accepted`, fills as possible, then `Cancelled(NoLiquidity)` for the rest |
| Modify, same price, total qty not increased | In place; **keeps** queue priority (an unchanged modify is accepted as a no-op) |
| Modify, total qty increased or price changed | Cancel/replace; **loses** priority; a new price that crosses matches immediately, with the modified order as aggressor |
| Modify to total qty <= filled (including 0) | Order cancelled: `Cancelled(ReducedToFilled)` |
| Modify to qty < 0 | `Rejected(InvalidQuantity)`, order untouched |
| Price outside the configured band | `Rejected(PriceOutOfBand)`. The band is an engine config shared by all implementations, like an exchange price collar. Market orders ignore price |
| Self-trade prevention | Out of scope (known limitation) |

### Modify quantity means total order quantity (M2)

`Modify.qty` is the new **total** order quantity, including anything already
filled (FIX `OrderQty` semantics), so `leaves = qty - filled`.
- Why: it is safe under a race with a fill. A client that sends "10 → 8"
  while 3 lots are filling ends up with at most 8 lots of total exposure. With
  "new remaining" semantics, the same message would leave 8 open on top of the
  3 already filled, 11 in all, more than the client asked for.
- Consequence: "decrease keeps priority" means the total decreases, which is
  equivalent to leaves decreasing by the same amount.
- Negative quantities are rejected rather than treated as a cancel. Zero is a
  meaningful "reduce to nothing"; a negative number is malformed input. This
  narrows the M1 default ("qty <= 0 → cancel") to match the requirement to
  reject negative quantities.

### Reject reasons and validation order (M2)

| Reason | When |
|---|---|
| `InvalidQuantity` | new order qty <= 0; modify qty < 0; any qty > `kMaxOrderQty` (1e9) |
| `PriceOutOfBand` | limit (or modify) price outside `[min_price, max_price]` |
| `DuplicateOrderId` | id belongs to a **live** (resting) order |
| `UnknownOrderId` | cancel/modify of an id that is not live |
| `CapacityExceeded` | a limit order arrives while `max_orders` orders are resting |

All implementations must check in the same order, because a single bad
command can fail several checks:
- New order: quantity → price → duplicate id → capacity.
- Modify: unknown id → quantity → price.

Notes:
- **Order IDs** are client-supplied arbitrary `uint64`, so lookup needs a hash
  map rather than a direct index.
- **Duplicate IDs** are checked against live orders only; an id can be reused
  once its order has filled or been cancelled. Checking "ever seen" would need
  unbounded memory.
- **`kMaxOrderQty` = 1e9** keeps every per-order quantity below 2^32, so the
  optimized engines can store quantities in 32 bits. It's an engine limit, not
  a business rule, but every implementation must enforce it identically.
- **Capacity is checked up front**, even for a limit order that would fully
  fill. Otherwise whether an order is accepted would depend on what happens to
  be resting in the book. Market orders never rest, so capacity doesn't apply
  to them.

### Event model (M2)

Event types: `Accepted`, `Rejected`, `Trade`, `PartiallyFilled`, `Filled`,
`Cancelled`, `Modified`. The order of events is part of the contract (see
`include/lob/events.hpp`):

```
NewOrder  → Accepted | Rejected
            per match: Trade, <resting status>, <aggressor status>
            market only: Cancelled(NoLiquidity) for any remainder
Cancel    → Cancelled | Rejected
Modify    → Rejected | Cancelled(ReducedToFilled)
          | Modified [, per-match sequence if priority was lost and the price crosses]
<status>  = PartiallyFilled{leaves} or Filled
```

- **One `Trade` per match, plus a status for each side.** This mirrors FIX,
  where every fill produces an execution report for each counterparty. It
  costs three sink calls per match instead of one. The optimized engines pay
  that cost too, so the benchmarks compare like with like.
- **Events carry no sequence number.** Their order is the contract. Stamping
  sequence numbers is the publisher's job when it serialises the stream, and
  in tests the vector index is the sequence number. This changes the M1
  architecture note, which put sequence numbers in events.
- **`Modified.kept_priority`** is in the event. Downstream consumers (and the
  tests) can see whether the queue position survived without re-deriving it.

### Interface every book implements (M2)

`OrderBook` concept (`include/lob/book_api.hpp`):
- `submit(NewOrder)`, `cancel(Cancel)`, `modify(Modify)`
- `best_bid()`, `best_ask()`, `order_count()`
- `snapshot()`: a full order-level view of the book, used to compare state
  between implementations, not just events.

The unit tests are GoogleTest **typed tests** over a list of implementations
(`tests/unit/book_test_support.hpp`). The behaviour spec is written once;
designs A and B are added to `BookImplementations` in M3 and M4.

Mutation check: deliberately planted bugs (unchanged modify losing priority,
touch price not crossing, LIFO queues, capacity off by one) each made between
1 and 9 tests fail. In other words, the suite does detect these bug classes.

## Architecture

```
            ┌──────────────────────────────────────────────┐
 commands → │ MatchingEngine<Levels, Sink>                 │ → Sink (events)
            │   validation · matching loop · modify rules  │
            │   OrderPool   (slab + index free-list)       │
            │   IdMap       (open addressing, OrderId→idx) │
            │   Levels = MapLevels | ArrayLevels           │
            └──────────────────────────────────────────────┘
 ReferenceBook: separate, deliberately naive implementation used only as a test oracle
```

**One matching implementation, templated on the price-level container.**
When designs A and B are benchmarked, the level container is the only thing
that differs.
- Rejected: a virtual `ILevels` interface. It costs an indirect call per
  operation and blocks inlining.
- Rejected: two independent engines. That doubles the correctness surface and
  makes the benchmark compare two implementations instead of two data structures.

**The reference book shares nothing with the fast engine except the input and
event types.** It uses `std::map<Price, std::deque<Order>>` plus
`std::unordered_map`. A differential test is only meaningful if the two
implementations can't share a bug.

**Events go through a template `Sink` parameter.** The call is static, with no
virtual dispatch and no allocation. `VectorSink` records events for tests;
`CountingSink` is used in benchmarks.

**Determinism.** Events carry no wall-clock time (and no sequence number; see
"Event model"). Time priority means arrival order.

**Strong types (M2).** `Price`, `Qty` and `OrderId` are distinct
`StrongInt<int64/uint64, Tag>` wrappers. Passing a quantity where a price is
expected becomes a compile error, and arithmetic is opt-in per tag (ids can't
be added). The wrapper is a trivially copyable 8-byte value, so it costs
nothing once optimised.
- Rejected: plain `using Price = int64_t`. It gives no protection against
  swapped arguments, which is exactly the bug class
  `bugprone-easily-swappable-parameters` warns about.

## Hot-path data structures (M3)

Every layout below is enforced with `static_assert` in the header that
defines it.

### Order: 32 bytes, 32-byte aligned (`include/lob/engine/order_pool.hpp`)

| offset | size | field | notes |
|---:|---:|---|---|
| 0 | 8 | `id` | needed for `Trade.resting_id` and to erase from the id map on fill |
| 8 | 4 | `leaves` | open quantity (< 2^32 because qty <= 1e9) |
| 12 | 4 | `total` | total order qty incl. filled; `filled = total - leaves` |
| 16 | 4 | `prev` | pool index; `kNil` = head of level |
| 20 | 4 | `next` | pool index; `kNil` = tail; doubles as the free-list link |
| 24 | 4 | `level` | handle of the level the order rests on |
| 28 | 1 | `side` | sits in what would otherwise be padding |
| 29 | 3 | padding | |

- **Two orders per 64 B line** (four per 128 B line on Apple Silicon). With
  32-byte alignment, an order never straddles a cache line.
- **32-bit pool indices instead of pointers.** Half the size, and the pool is
  one contiguous allocation.
- **The price is not stored in the order.** The M1 plan had it there; M2's
  total-quantity modify semantics added a field and pushed the struct past
  32 B. The price is reachable through `level` in one hop, and every path that
  needs it (cancel, modify, fill) touches the level anyway. The `level` handle
  also makes cancel O(1) in both designs: unlinking never searches for the level.
  - Rejected: storing a 32-bit price offset within the band. It also fits, but
    it constrains the band width for design A, and cancel would still need a
    level lookup.
  - Rejected: a hot/cold split with `total` in a parallel array. It saves
    nothing here, since everything fits in 32 B.

### Level: 32 bytes (`include/lob/engine/level.hpp`)

Fields: `price` (8) · `total` (8, 64-bit because many orders × 1e9 overflows
32 bits) · `head` (4) · `tail` (4) · `count` (4) · `side` (1) · padding (3).

### Containers

- **OrderPool** is a fixed slab with an intrusive LIFO free-list, built and
  touched in the constructor. The most recently freed slot is reused first,
  since it's the one most likely to still be in cache. **Only orders that rest
  take a slot**; an incoming order is matched as a stack-local `Aggressor`.
- **IdMap**: linear probing over 16-byte `{key, value}` slots, power-of-two
  capacity >= 2 × max orders (load factor <= 0.5), and the murmur3 `fmix64`
  hash so sequential ids spread evenly.
  - Order ids can be any `uint64`, including 0 and `UINT64_MAX`, so an empty
    slot is marked by the value (`kNil`), not by a reserved key.
  - Deletion uses **backward shift**: pull later entries of the cluster back
    into the hole. With tombstones, probe lengths slowly decay under
    add/cancel churn, which forces periodic rehashes.
  - Tested against `std::unordered_map` at full load in a deliberately tiny
    table, so long clusters and wrap-around are common.
  - Rejected: `std::unordered_map`. It allocates a node per insert and chases
    a pointer on every lookup.
  - Rejected: `absl::flat_hash_map`. It's excellent, but adds a dependency
    and is less explainable line by line.
- **Market orders** are matched as a limit order at the most aggressive price
  (`INT64_MAX` for a buy, `INT64_MIN` for a sell), so the matching loop has no
  market/limit branch.

### Design A: `MapLevels` (`include/lob/engine/map_levels.hpp`)

- One `std::map<Price, LevelRef>` per side: `std::greater<>` for bids and
  `std::less<>` for asks, so `begin()` is always the best level and costs O(1)
  (std::map caches its leftmost node).
  - Rejected: one map using `rbegin()` for bids. In libc++, `--end()` walks
    down from the root, which is O(log L).
- The map stores only a 32-bit handle. `Level` structs live in a separate
  contiguous pool, so orders can point at their level.
- Map nodes come from `BlockArena` through `ArenaAllocator`. The arena is a
  fixed-block free list allocated (and page-faulted) up front, using 64-byte
  blocks; the red-black tree node is 48 B in both libc++ and libstdc++, and
  the allocator throws on mismatch.
  - Rejected: `std::pmr::unsynchronized_pool_resource`. It can fall through
    to its upstream allocator, and pmr adds a virtual call per allocation.
- **Cost model:** creating or erasing a level is O(log L) (tree insert/erase);
  best-price lookup and cancel are O(1).
- **Memory bound:** a level exists only while an order rests on it, so there
  are never more than `max_orders` levels or map nodes.

### Design B: `ArrayLevels` (M4)

`ArrayLevels` is a flat `Level[band]` indexed by
  `price - band_low`, plus a two-level occupancy bitmap, so finding the next
  non-empty level after the best one empties takes two `countl_zero` /
  `countr_zero` instructions instead of a linear scan. It will be benchmarked
  against the plain linear scan (**provisional**: the winner depends on how
  sparse the book is).

## Verification (M3)

### Differential testing

`workload/include/lob/workload/differential.hpp` runs the reference book and
an engine in lockstep on one random command stream. It checks:
- after **every** command: the two event streams are identical;
- every N commands and at the end: the full order-level snapshots match, best
  bid/ask and order count match, and `check_invariants()` passes. That
  function walks every level and order, checking links, aggregates, sides,
  id-map entries and pool counts.

A failure report includes the seed, the failing command's index, both event
streams (or the first differing level) and the last 24 commands, plus a
`lob_fuzz` command line that reproduces it exactly.

The generator tracks which orders are live by observing the reference book's
events (Accepted adds an id; Filled and Cancelled remove it). That lets cancels
and modifies mostly target real resting orders, while a configurable fraction
target unknown ids, reuse dead ids, duplicate live ids, or carry invalid
quantities and prices. There are four named configurations:

| config | band | capacity | character |
|---|---|---|---|
| `dense` | 64 ticks | 64 | constant level collisions, sweeps, band edges |
| `default` | 100k ticks | 4,096 | baseline mix |
| `edge` | 16 ticks | 8 | ~10% invalid input of every kind, modify-heavy, capacity pressure |
| `deep` | 1M ticks | 50,000 | add-heavy, deep book, long sweeps, saturates capacity |

Over 200k commands (seed 1), every configuration produced trades, partial
fills, both kinds of modify, and every cancel and reject reason that its
shape allows. `CapacityExceeded` appears in `edge` and `deep`; `NoLiquidity`
in `dense` and `edge`.

The harness is itself tested: an engine with a planted bug must be reported,
with a reproduction line.

**Results (M3, design A):**
- CI runs 12 cases × 200k commands = 2.4M per build, under release and
  ASan+UBSan on GCC 14, Clang 18 and Apple Clang.
- Locally (Apple M4 Max), `lob_fuzz --config all --seeds 1-25 --ops 1000000`
  matched the reference on **100,000,000 commands** (release, 18.2 s).
- A further **10,000,000** commands (seeds 101–105) passed under ASan+UBSan.

`replay_config` adds one more generator knob, `target_live_orders`. It scales
the cancel weight by `live / target`, so each order has a constant cancel
hazard and the book holds a steady depth. Without it, the benchmark workload
drained to about 6 resting orders (see `docs/benchmarks.md`).

**Reproducibility across platforms.** The RNG (xoshiro256**) and the integer
distributions are hand-written, because `std::uniform_int_distribution`
gives different sequences in libc++ and libstdc++.
- Writing `GeneratorGoldenSequence` found a real bug. Two expressions drew
  several random numbers at once (`f(rng(), rng())`, `rng() * rng()`), and the
  evaluation order of such operands is unspecified in C++. GCC and Clang could
  therefore produce different streams from the same seed.
- Every draw is now its own statement. The golden test pins a fingerprint of
  the first 20k commands of two workloads, and CI checks it under both GCC
  (libstdc++) and Clang (libc++).

### No allocation on the hot path

`tests/alloc/no_alloc_test.cpp` replaces global `operator new`/`delete` with
counting versions. It pre-generates a workload, constructs the engine, then
switches counting on and replays 200k commands. The engine must make **zero**
allocations.
- It runs for every named workload, so rejects, sweeps, modifies, level
  create/erase, capacity exhaustion and id reuse are all covered.
- This is stronger than "no allocation after warm-up": the count is zero from
  the very first command after construction.

### API note

`lob::apply(book, command)` was renamed to `lob::process`. `Command` is a
`std::variant`, so an unqualified `apply(book, cmd)` also finds `std::apply`
through argument-dependent lookup, and for a non-const command `std::apply`
is the better match (a compile error).

## Threading

The matching core is single-threaded, for these reasons:
- Matching is inherently sequential: every order depends on the book state the
  previous one left behind.
- A lock or atomic on a contended cache line costs tens of ns, about as much
  as a whole matching operation.
- Real engines run one pinned thread per instrument or partition and scale
  across symbols. Parallelism belongs at the edges (gateway, parsing,
  publishing), connected by SPSC queues.

## Platform notes

- **Cache-line size** is 128 B on Apple Silicon and 64 B on x86-64. It's a
  single constant, `lob::kCacheLine`. `std::hardware_destructive_interference_size`
  is avoided because GCC warns that its value is ABI-unstable.
- **Timer resolution.** On Apple Silicon, `mach_absolute_time` and
  `cntvct_el0` tick at 24 MHz, about 41.7 ns per tick. That is too coarse to
  time a single sub-100 ns operation, so per-op percentiles measured on macOS
  are coarse. Headline latency numbers come from Linux x86-64 using `rdtsc`.
- **CPU pinning.** macOS doesn't support hard thread affinity on Apple
  Silicon; the best available is QoS `USER_INTERACTIVE`. Linux uses
  `pthread_setaffinity_np` plus `isolcpus`, a fixed frequency governor, and
  turbo disabled; all of these are documented with the results.
- **Docker on macOS** runs inside a Linux VM, so `perf` has no hardware
  counters there. Profiling numbers come from bare-metal Linux.

## Tooling choices

- GoogleTest and Google Benchmark via `FetchContent`, pinned to exact release
  tarballs with SHA-256 hashes. No system packages are needed, so every
  machine builds identical code.
- Sanitizers are applied globally, so the dependencies are instrumented too.
  Mixing instrumented and uninstrumented code gives false positives.
- The `asan-ubsan` preset builds Debug at `-O1`: asserts stay on, and large
  differential runs remain fast enough. `-fno-sanitize-recover=all` makes any
  UB report fail the test.
- clang-tidy runs only on project targets; `.clang-tidy` documents every
  disabled check. CI pins the LLVM major version to match local development
  (clang-format output differs between majors).
