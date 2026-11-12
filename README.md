# lob-engine

A low-latency limit order book and matching engine in C++20: price-time priority,
no heap allocation on the hot path, two benchmarked price-level designs, and
differential testing against a deliberately simple reference implementation.

> **Status: Milestone 2.** The reference (oracle) order book and the full behaviour
> test suite are in place; the optimized engines land in Milestones 3–4. Design decisions are
> tracked in [docs/design.md](docs/design.md). No performance numbers are published
> yet; all numbers will come from documented, reproducible runs.

## Building

Requirements: CMake ≥ 3.25, Ninja, a C++20 compiler (Clang ≥ 18, GCC ≥ 14, Apple Clang ≥ 16).
Dependencies (GoogleTest, Google Benchmark) are fetched and pinned automatically.

```bash
cmake --workflow --preset release      # configure + build + test
cmake --workflow --preset asan-ubsan   # AddressSanitizer + UBSan
cmake --workflow --preset tidy         # clang-tidy over all project code
cmake --build build/release --target format        # apply clang-format
cmake --build build/release --target format-check  # CI formatting gate
```

| Preset | Purpose |
|---|---|
| `debug` | `-O0 -g`, asserts on |
| `release` | `-O3`, portable (what CI runs) |
| `release-native` | `-O3 -march/-mcpu=native`; used for published benchmarks |
| `relwithdebinfo` | `-O3 -g -fno-omit-frame-pointer`; used for perf / Instruments |
| `asan-ubsan` | `-O1 -g` + ASan + UBSan, asserts on |
| `tidy` | Debug build with clang-tidy as a compile step |

Run only unit tests, or only the benchmark smoke run:

```bash
ctest --preset release --label-exclude bench
ctest --preset release --label-regex bench
```

On macOS, `clang-format`/`clang-tidy` come from Homebrew LLVM (`brew install llvm`);
the build finds them even though that keg is not on `PATH`.

### Linux (Docker)

```bash
docker/run.sh cmake --workflow --preset release                       # GCC 14
CC=clang-18 CXX=clang++-18 docker/run.sh cmake --workflow --preset asan-ubsan
```

Linux build trees live in a Docker volume (one per compiler), separate from the
host's `build/`. Final benchmark and `perf` numbers come from bare-metal Linux, not
from Docker on macOS (see [docs/design.md](docs/design.md#platform-notes)).

## Layout

```
include/lob/   core engine (header-only: types, pool, id map, price levels, engine, sinks)
reference/     simple reference order book (test oracle)
tests/         unit/ and differential/ tests (GoogleTest)
bench/         Google Benchmark suites + latency histogram harness  [M5]
docker/        Linux build/profiling image
docs/          design decisions, benchmark results
```

## Roadmap

1. ✅ Architecture + repo skeleton
2. ✅ Reference order book + full unit tests
3. Optimized engine, design A (map-based levels) + differential tests
4. Optimized engine, design B (flat tick array + occupancy bitmap) + differential tests
5. Benchmark suite, latency histograms, A vs B comparison
6. Profiling pass: hotspots fixed, before/after documented
7. Final README: design doc, methodology, results, limitations, next steps
