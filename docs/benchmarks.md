# Benchmark log

Every number in this file comes from a recorded run. The raw Google Benchmark
JSON is in [`docs/results/`](results/). Each entry states the hardware, OS,
compiler, flags and workload. Numbers from different workloads or machines
are not comparable.

## M3: replay throughput, reference vs. design A (2026-11-11)

**What is measured.** One pre-generated stream of 1,000,000 commands
(`replay_config`, seed 42) is replayed into a freshly constructed book in each
iteration; construction and destruction are excluded from timing. Both books
emit into `CountingSink`, which records nothing and never allocates. Dispatch
is identical for both: `lob::process`, one `std::visit` per command.

**Workload shape** (measured while generating it against the reference book):

| | |
|---|---|
| command mix (weights) | 45 limit · 5 market · 40 cancel (scaled by depth) · 10 modify |
| prices | passive 1–25 ticks behind a random-walk mid; 10% priced 0–3 ticks through it |
| steady-state depth | avg 5,575 resting orders (sampled every 1k commands); 218 price levels at the end |
| output | 1,985,960 events, 328,653 trades |
| invalid input | ~0.5% of commands |

**Environment**

| | |
|---|---|
| CPU | Apple M4 Max (10 performance + 4 efficiency cores), 36 GB |
| OS | macOS 27.2.1 |
| Compiler | Apple clang 21.0.0 |
| Flags | `-O3 -DNDEBUG -march=native` (`release-native` preset) |
| Pinning | none: macOS has no hard thread affinity on Apple Silicon |
| Load average at start | 1.41 (desktop session; Docker Desktop VM idle in background) |
| Repetitions | 10 × ≥ 1 s, median reported |

**Results** ([raw JSON](results/m3_replay_apple_m4max.json))

| Book | median time / 1M cmds | ns per command | commands / s | CV |
|---|---:|---:|---:|---:|
| Reference (`std::map` + `std::deque` + `std::unordered_map`) | 62.1 ms | 62.1 | 16.1 M | 0.48% |
| Design A (`MapEngine`) | 28.3 ms | 28.3 | 35.4 M | 0.54% |

Design A is **2.2× faster than the reference** on this workload.

**How to read this**

- This is **throughput, averaged over a mix**. "ns per command" is total time
  divided by commands; it is not the latency of any single operation. A
  command that sweeps five levels and a rejected cancel count the same.
  Per-operation latency distributions (p50 to max) arrive with the M5 harness.
- The reference is not a strawman. It's a reasonable "first version" built
  from standard containers, and at this depth its maps stay small.
- **Noise sources on this run:** no CPU pinning; macOS may migrate the thread
  between P-cores; there's a background desktop session; and the replayed
  command stream (~40 MB) streams through memory on every iteration. Run-to-run
  spread was under 1% (CV), so the 2.2× difference is far outside the noise.
- **An earlier, discarded run.** Before the depth model was added, the replay
  workload drained the book to about 6 resting orders. That run measured
  33.3 ns (design A) vs. 63.0 ns (reference). It's recorded here only to
  explain why `target_live_orders` exists: a benchmark on an empty book
  flatters simple data structures.

**Reproduce**

```bash
cmake --preset release-native && cmake --build --preset release-native --target lob_replay_bench
./build/release-native/bench/lob_replay_bench --benchmark_repetitions=10 \
    --benchmark_report_aggregates_only=true --benchmark_min_time=1s
```

## M4: design B vs. design A, and bitmap vs. linear scan (2026-11-12)

Same harness, environment and flags as M3 (Apple M4 Max, macOS 27.2.1,
Apple clang 21, `-O3 -march=native`, no pinning; load average about 1.2 at
start). 10 repetitions, median reported. Raw JSON:
[replay](results/m4_replay_apple_m4max.json),
[next-best search](results/m4_best_search_apple_m4max.json).

### Replay throughput: two workload shapes

| workload | shape (measured) |
|---|---|
| **tight** (`replay_config`, same stream as M3) | avg 5,575 resting orders; 218 levels at end; prices within ~25 ticks of the mid |
| **sparse** (`replay_sparse_config`) | avg 1,802 resting orders; 1,661 levels at end; orders spread over ~5,000 ticks of a 1M-tick band |

| Book | tight: ns/cmd | tight: CV | sparse: ns/cmd | sparse: CV |
|---|---:|---:|---:|---:|
| Reference | 72.8 | 3.5% | 107.0 | 0.4% |
| Design A: `MapEngine` | 30.3 | 2.2% | 58.8 | 0.1% |
| Design B: `ArrayEngine` (bitmap) | 19.6 | 0.5% | 22.5 | 0.3% |
| Design B: `ArrayScanEngine` (linear) | 19.0 | 0.6% | 24.0 | 0.4% |

**What it shows**

- **Design B is 1.5× faster than design A on the tight book and 2.6× faster
  on the sparse one.** Design A's costs grow with the number of levels (a
  tree walk on every level create/erase, and nodes scattered in memory).
  Design B's cost barely moves: 19.6 → 22.5 ns while the level count grows
  about 7×.
- **Bitmap vs. linear scan is a wash on these workloads.** It's within 3% on
  tight (linear slightly ahead) and 7% in the bitmap's favour on sparse.
  - Even the sparse book has an occupied level every few ticks near the top,
    so a linear scan reads only a few adjacent cache lines.
  - The bitmap does a little extra work on every insert and erase (two word
    updates) whether or not a search happens.
- **Session-to-session drift.** The tight stream is the same one measured in
  M3, where the reference ran at 62.1 ns and design A at 28.3 ns. This
  session ran 7–17% slower, more than the within-run CV, on an unpinned
  desktop machine. Compare numbers only within one table.

### Isolated: cost of finding the next best level vs. gap size

64 bid levels `gap` ticks apart; each iteration empties the best level
(forcing one next-best search across `gap` empty ticks) and re-creates it.

| gap (ticks) | 1 | 4 | 16 | 64 | 256 | 1,024 | 4,096 |
|---|---:|---:|---:|---:|---:|---:|---:|
| Bitmap (ns/iter) | 2.5 | 2.6 | 2.6 | 3.0 | 3.0 | 3.0 | 3.2 |
| Linear (ns/iter) | 6.1 | 6.8 | 15.5 | 26.6 | 81.2 | 257 | 987 |

The linear scan costs about 0.24 ns per empty tick (987 ns / 4,096): a
sequential, prefetch-friendly walk, but still O(gap). The bitmap is flat,
under 3.2 ns for every gap tested.

**Conclusion for design B.** Keep the bitmap. On realistic books it costs
nothing measurable, and it bounds the worst case. When the top of the book
is swept and the next level is far away (a fast market, a thin instrument,
after a large market order), a linear scan would add hundreds of ns to
exactly the command where latency matters most. The replay averages hide
this because such events are rare; per-operation tail latency (M5) is where
it should show up.

## M5: per-operation latency distributions and throughput (2026-11-12)

### Method

`bench/latency/lob_latency.cpp`, run separately for each (engine, workload)
pair:

1. **Generate** the command stream once, against the reference book
   (untimed).
2. **Latency runs** (5): construct a fresh engine and replay 200k commands
   untimed as warm-up. Then timestamp every one of the next 2M commands
   individually and record it in a log-linear histogram for its category.
   - Categories: passive limit, aggressive limit (traded), market, cancel,
     modify.
   - Histograms merge across runs, giving 10M timed commands per pair.
3. **Throughput runs** (5): construct a fresh engine and warm up the same
   way. Time the same 2M commands as one block, with no per-op timer.
   Median reported.

The histogram (`workload/include/lob/workload/latency_histogram.hpp`) is
HdrHistogram-style:
- values below 128 ticks are exact; above that the relative error is at most
  1/64;
- percentiles report the bucket's upper edge, so they never under-report;
- max is exact.

Unit tests check its percentiles against exact sorted values.

### Workloads

| workload | model | measured shape |
|---|---|---|
| **realistic** | 48% limit · 42% cancel (scaled by depth, target 5k) · 3% market · 7% modify; passive prices geometric from the touch (mean ~4 ticks); 6% of limits aggressive | avg 2,624 resting orders, 89 levels at end |
| **thin** | 50% limit · 37% cancel (target 400) · 8% market (up to 200 lots) · 5% modify; passive mean ~33 ticks out | avg 430 resting orders, 167 levels at end |

### Environment and timer

| | |
|---|---|
| CPU / OS | Apple M4 Max, macOS 27.2.1 (load average 1.9–2.0 during the run) |
| Compiler / flags | Apple clang 21, `-O3 -DNDEBUG -march=native` (`release-native`) |
| Thread placement | **no hard pinning** (macOS has none on Apple Silicon); QoS `USER_INTERACTIVE` |
| Clock | `cntvct_el0` after `isb`. Reports 1 GHz units but **advances in ~42 ns steps** (measured median non-zero step: 42.0 ns) |
| Timer overhead | back-to-back reads: mean 11.4 ns (included in every latency below; not subtracted) |

Raw results: [m5_latency_apple_m4max.json](results/m5_latency_apple_m4max.json).

### Throughput (block-timed, median of 5)

| Book | realistic: ns/cmd | realistic: M cmd/s | thin: ns/cmd | thin: M cmd/s |
|---|---:|---:|---:|---:|
| Reference | 53.4 | 18.7 | 57.2 | 17.5 |
| Design A (`MapEngine`) | 24.2 | 41.3 | 29.8 | 33.6 |
| **Design B (`ArrayEngine`)** | **18.9** | **52.9** | **19.8** | **50.5** |
| Design B, linear scan | 18.5 | 54.1 | 19.3 | 51.9 |

### Per-operation latency, 10M timed commands each (ns)

Every value includes the ~11 ns timer overhead. Percentiles are quantized to
the 42 ns clock step (42, 83/84, 125, 167, ...).

**realistic**

| Book | mean | p50 | p90 | p99 | p99.9 | max |
|---|---:|---:|---:|---:|---:|---:|
| Reference | 65.7 | 42 | 125 | 167 | 251 | 26,185 |
| Design A | 47.2 | 42 | 84 | 125 | 167 | 25,476 |
| Design B | 42.0 | 42 | 83 | 125 | 167 | 28,933 |
| Design B, linear | 44.6 | 42 | 84 | 125 | 167 | 25,476 |

**thin**

| Book | mean | p50 | p90 | p99 | p99.9 | max |
|---|---:|---:|---:|---:|---:|---:|
| Reference | 67.6 | 83 | 84 | 167 | 251 | 21,933 |
| Design A | 49.4 | 42 | 84 | 125 | 167 | 27,850 |
| Design B | 40.4 | 42 | 83 | 125 | 167 | 25,541 |
| Design B, linear | 42.1 | 42 | 83 | 125 | 167 | 28,475 |

**Mean latency by category, realistic (ns):**

| Book | passive limit | aggressive limit | market | cancel | modify |
|---|---:|---:|---:|---:|---:|
| Reference | 46.6 | 67.1 | 67.9 | 91.6 | 83.7 |
| Design A | 50.5 | 54.9 | 57.0 | 37.5 | 43.4 |
| Design B | 44.9 | 52.8 | 54.1 | 32.4 | 34.5 |

### How to read these numbers

- **Latency and throughput measure different things.** Design B's mean timed
  latency (42 ns) is about twice its block-timed cost (18.9 ns/cmd).
  - The fenced timestamps (`isb`) drain the pipeline, so each command runs
    alone.
  - Without timestamps, the out-of-order core overlaps consecutive commands.
  - Timed latency is the cost when a command arrives alone (the market-data
    case). Throughput is the cost in a busy stream (the replay or catch-up
    case).
- **On this machine the percentiles are dominated by clock resolution.** All
  three optimized books show p50 = 1 clock step and p99 = 3 steps. The
  differences between them live below the 42 ns step, which is why the mean
  is reported: averaged over millions of samples, quantization error cancels
  out (each measurement starts at a random point within a tick).
- **The max is the operating system, not the engine.** Every book, including
  the naive reference, shows a 6–29 µs maximum: interrupts, preemption or
  migration on an unpinned desktop thread. Bounding it needs an isolated core
  (`isolcpus`, `nohz_full`, IRQ affinity) on Linux, which macOS can't provide.
- **The cancel column shows the biggest gap.** The reference's cancel (91.6 ns)
  is a hash lookup plus a linear search of a `std::deque` within the level.
  The engines' cancel (32–38 ns) is a hash lookup plus an O(1) intrusive
  unlink.
- **The linear-scan risk didn't show up in these workloads.** Design B with a
  linear scan matches the bitmap version here because even the thin book's
  gaps after a sweep stay small. The cost of a large gap is real but rare;
  M4's isolated benchmark measures it at about 1 µs for 4,096 empty ticks.
  This workload doesn't produce that case, and it hasn't been tuned until it
  does.

### Noise sources (and what was done about each)

| Source | Effect | Mitigation here | On a dedicated Linux box |
|---|---|---|---|
| Clock resolution | percentiles quantized to 42 ns | report means; document | TSC: ~0.3 ns resolution |
| Timer overhead / serialization | ~11 ns + lost overlap per sample | measured and reported; throughput measured separately | same approach |
| No CPU pinning | migration between cores, cold caches | QoS `USER_INTERACTIVE` (P-cores only) | `--cpu N` with `isolcpus` |
| Interrupts / preemption | µs-scale max | none possible | `nohz_full`, IRQ affinity away from the core |
| Frequency scaling / thermals | run-to-run drift (7–17% between M3 and M4 sessions) | repetitions; compare only within one run | fixed governor, turbo off |
| Page faults | first-touch stalls | all engine memory allocated and touched at construction; warm-up | plus `mlockall` |
| Background load | load average ~2 | none | idle machine |

### Reproduce

```bash
cmake --preset release-native && cmake --build --preset release-native --target lob_latency
./build/release-native/bench/lob_latency --ops 2000000 --warmup 200000 --runs 5 \
    --json latency.json            # add --cpu N on Linux
```

Headline numbers belong on a bare-metal x86-64 Linux machine with an
isolated core. The harness pins the thread (`--cpu`) and uses `rdtsc` there;
nothing in this file was measured on such a machine.

## M6: profiling pass, hotspots and fixes (2026-11-12)

### Tooling

- **Profiler:** macOS `sample` (statistical, 1 ms) on a `relwithdebinfo`
  build (`-O3 -g -fno-omit-frame-pointer`). Xcode's `xctrace` wasn't on the
  developer path.
- **Process being profiled:** `lob_latency --mode throughput`. This mode has
  no per-op timestamps, so the profile isn't distorted by `isb`.
- **Raw profiles:** [before](results/m6/before_sample_array_realistic.txt),
  [after](results/m6/after_sample_array_realistic.txt).
- **Method:** every change was measured with **interleaved A/B runs**.
  Separate binaries for each variant run alternately (A, B, C, A, B, C, …)
  for 4 rounds × 5 runs, so session drift hits all of them equally. Raw data
  and scripts are in [results/m6/ab/](results/m6/ab/).

### Methodology lesson: a single before/after comparison misled

The first "after" run for change A showed design B going from 20.6 to
18.6 ns/cmd (−10%). The interleaved comparison of the same two binaries
showed −1.4%. Most of the apparent gain was session drift, of the same kind
seen between M3 and M4. From here on, every number in this section comes from
interleaved runs.

### Profile before (design B, realistic workload; share of 8,341 samples)

| Share | Function |
|---:|---|
| 26.1% | `match` (matching loop) |
| 14.7% | `submit` |
| 12.7% | cancel handler (inlined into `std::visit` dispatch) |
| 9.1% | `remove` (unlink + id erase + pool release) |
| 6.7% | new-order dispatch |
| 6.7% | `modify` |
| 3.3% | `ArrayLevels::get_or_insert` |
| 3.1% | `IdMap::insert` |

A function-level profile doesn't say *why* `match`, `submit` and cancel are
expensive. Each of them touches the id map, so the id map was the first
hypothesis.

### Hypothesis 1: the id map's memory footprint (confirmed)

With `max_orders = 1M`, the old id map had 2M slots of 16 B (33.6 MB), and
its hash scattered even the ~2,600 live orders across all of it. Experiment:
the same command stream, engines built with smaller capacity (so a smaller
table), live orders unchanged.

| max_orders (id table) | design A | design B |
|---|---:|---:|
| 1,048,576 (33.6 MB) | 24.5 | 19.3 |
| 65,536 (2.1 MB) | 21.9 | 15.5 |
| 16,384 (0.5 MB) | 23.4 | 16.9 |

ns/cmd, median of 9. A **19% throughput difference from table size alone.**
The table's cache footprint, not its algorithm, was the top cost.

### Changes, each measured (interleaved, 20 runs per cell, ns/cmd median)

| Change | Design B realistic | Design B thin | Design A realistic | Design A thin |
|---|---:|---:|---:|---:|
| baseline | 19.08 | 19.81 | 24.43 | 29.65 |
| **A** single-probe insert/erase | −1.5% | −2.8% | −1.5% | −1.2% |
| **A+B** + Fibonacci hash | −3.1% | −4.0% | −3.6% | −3.4% |
| **A+B+C** + index-only slots | **13.46 (−29.4%)** | **14.92 (−24.7%)** | **19.98 (−18.2%)** | **26.06 (−12.1%)** |

The spread (max − min over median) was 3–5% for every final cell.

- **A. Single probe.** `IdMap::probe` returns either the slot holding the id
  or the empty slot where it would go.
  - Cancel and modify erase that slot directly instead of re-probing.
  - A passive add inserts into the slot found by its own duplicate check.
    This is only safe when matching erased nothing in between, because a
    backward-shift erase can move entries. The engine therefore reuses the
    slot only when the order didn't trade.
  - Small, consistent gain.
- **B. Fibonacci hashing.** `(key × 2^64/φ) >> (64 − log2 capacity)`, one
  multiply, replacing murmur3's `fmix64` (two multiplies, three shift-xors)
  on the critical path of every map access. Small, consistent gain.
- **C. Index-only slots.** A slot now holds just the 4-byte pool index; key
  comparisons read the id from `orders_[index].id` through a statically
  dispatched `key_of` callable.
  - The table is 4× smaller (8.4 MB at 1M capacity), with 16 slots per cache
    line.
  - A hit costs no extra memory access, because the caller reads that
    order's cache line next anyway.
  - A miss usually lands on an empty slot without touching any order.
  - This is the big one.

**Capacity sensitivity after the changes.** This is the same experiment as
in Hypothesis 1, rerun with the new map:

| max_orders | design A before → after | design B before → after |
|---|---|---|
| 1,048,576 | 24.5 → 20.3 | 19.3 → 13.9 |
| 65,536 | 21.9 → 20.2 | 15.5 → 13.6 |
| 16,384 | 23.4 → 20.4 | 16.9 → 13.9 |

Throughput no longer depends on capacity, and at every size the new map
beats the old map at its best size. So C isn't only about footprint: a
smaller table also keeps a probe within one cache line that the operation
shares with work it does anyway.

Correctness: after each change the full test suite passed, and so did the
differential fuzzer (28.8M commands per change, all three engines, all
configs). The final code then matched the reference on a fresh campaign of
**300,000,000 commands** (seeds 101–125, 100M per engine).

### Profile after (share of 3,697 samples)

| Share | Function |
|---:|---|
| 31.0% | `match` |
| 13.4% | cancel handler (inlined) |
| 11.8% | `submit` |
| 8.7% | `remove` |
| 8.3% | new-order dispatch |
| 7.8% | `modify` |

`IdMap::insert` has dropped out of the top entries. The matching loop is now
the clear top cost. Remaining candidates, not attempted (each would need the
same A/B treatment):
- per-fill bookkeeping: three sink calls and an id-map erase per fill;
- prefetching the next resting order in a level while filling the current
  one;
- a structure-of-arrays split of `Order` so the fill loop touches only
  `leaves`/`next`.

### Before vs. after, full M5 measurement (same settings, same harness)

[M5 raw](results/m5_latency_apple_m4max.json) ·
[M6 raw](results/m6/after_latency_apple_m4max.json). Throughput is
block-timed (median of 5). Latency is per-op over 10M commands and includes
~11 ns of timer overhead; percentiles are quantized to the 42 ns clock step.

| Book / workload | throughput M5 → M6 | mean latency M5 → M6 | p99 M5 → M6 | p99.9 M5 → M6 |
|---|---|---|---|---|
| Design B / realistic | 18.9 → **13.3 ns** (75 M cmd/s) | 42.0 → 24.9 | 125 → 42 | 167 → 84 |
| Design B / thin | 19.8 → **14.8 ns** (68 M cmd/s) | 40.4 → 24.3 | 125 → 42 | 167 → 84 |
| Design A / realistic | 24.2 → 19.5 ns | 47.2 → 29.4 | 125 → 83 | 167 → 84 |
| Design A / thin | 29.8 → 25.8 ns | 49.4 → 34.1 | 125 → 84 | 167 → 125 |
| Reference / realistic | 53.4 → 53.3 ns | 65.7 → 62.7 | 167 → 167 | 251 → 251 |

The reference book doesn't use the engine's id map, and it's unchanged
within noise, which is a useful control. Maxima are still 14–52 µs for every
book, including the reference (OS noise; see M5).

One anomaly is left unexplained: in the M6 run, the linear-scan variant's
passive-limit p99 was 125 ns against 42 ns for the bitmap variant (and one
520 µs maximum). Passive adds never trigger a next-best search, so this is
most likely run-level noise (a run landing on a busy core). It isn't
attributed to the scan.
