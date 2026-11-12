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
