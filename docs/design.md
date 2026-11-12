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

## Hot-path data structures (layout finalized in M3 with static_asserts)

- **Order** is about 32 bytes. *M2 note:* total-quantity modify semantics
  mean each order also stores its filled (or total) quantity. Fitting into
  32 B therefore needs packing, e.g. a 32-bit price offset within the band, or
  folding the side into a spare bit. That gets decided and measured in M3. List links are `uint32` pool indices instead of
  pointers, which halves the size and lets the pool be one contiguous allocation.
- **OrderPool** is a fixed slab with a LIFO free-list, so recently freed slots
  (likely still in cache) are reused first. When it is full, the order is
  rejected with `CapacityExceeded`; the pool never grows on the hot path.
- **IdMap** uses linear probing, power-of-two capacity, and backward-shift
  deletion (no tombstones).
  - Rejected: `std::unordered_map`, which allocates a node per insert and
    chases pointers on lookup.
  - Rejected for now: `absl::flat_hash_map`. It's excellent, but adds a
    dependency and is harder to explain line by line.
- **Level** stores the head and tail of an intrusive FIFO list, plus total
  quantity and order count.
- **Design A (`MapLevels`)** is a `std::map` per side with a fixed-block node
  allocator pre-sized at startup, plus a cached best price. Note:
  `std::pmr::unsynchronized_pool_resource` can fall through to its upstream
  allocator, which is why the allocator is written by hand.
- **Design B (`ArrayLevels`)** is a flat `Level[band]` indexed by
  `price - band_low`, plus a two-level occupancy bitmap, so finding the next
  non-empty level after the best one empties takes two `countl_zero` /
  `countr_zero` instructions instead of a linear scan. It will be benchmarked
  against the plain linear scan (**provisional**: the winner depends on how
  sparse the book is).

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
