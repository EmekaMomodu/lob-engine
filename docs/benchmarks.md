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
