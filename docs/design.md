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
| Modify, quantity decrease | In place; **keeps** queue priority |
| Modify, quantity increase or price change | Cancel/replace; **loses** priority; a new price that crosses matches immediately |
| Modify to qty <= 0 | Treated as a cancel |
| Price outside the configured band | `Rejected(PriceOutOfBand)`. The band is an engine config shared by all implementations, like an exchange price collar |
| Self-trade prevention | Out of scope (known limitation) |

Reject reasons: `ZeroOrNegativeQty`, `UnknownOrderId`, `DuplicateOrderId`,
`PriceOutOfBand`, `CapacityExceeded`.

Order IDs are client-supplied arbitrary `uint64`, so lookup needs a hash map
rather than a direct index.

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

**Determinism.** Events carry an engine sequence number, never wall-clock
time. Time priority means arrival order.

## Hot-path data structures (layout finalized in M3 with static_asserts)

- **Order** is about 32 bytes. List links are `uint32` pool indices instead of
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
