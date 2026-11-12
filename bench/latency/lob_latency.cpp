// lob_latency: per-operation latency distribution and throughput per engine.
//
//   lob_latency                                   # all engines x realistic,thin
//   lob_latency --engine array --workload thin --ops 5000000 --runs 7 --cpu 3
//   lob_latency --json docs/results/m5_latency.json
//
// Method, per (engine, workload):
//   1. Generate the command stream once, against the reference book (untimed).
//   2. Latency runs (--runs): fresh engine; replay --warmup commands untimed;
//      then time every remaining command individually with TickClock and
//      record it in a histogram for its category. Histograms merge over runs.
//   3. Throughput runs (--runs): fresh engine; warm up; time the remaining
//      commands as one block (no per-op timer overhead). Median reported.
//
// Categories: passive limit (no trade), aggressive limit (traded), market,
// cancel, modify. Rejected commands count in their command's category.

#include "lob/book_api.hpp"
#include "lob/engines.hpp"
#include "lob/reference/reference_book.hpp"
#include "lob/sinks.hpp"
#include "lob/workload/latency_histogram.hpp"
#include "lob/workload/workload.hpp"
#include "tick_clock.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace {

using lob::workload::LatencyHistogram;

// Plain enum on purpose: categories index the histogram array.
// NOLINTNEXTLINE(cppcoreguidelines-use-enum-class)
enum Category : std::uint8_t { kPassive, kAggressive, kMarket, kCancel, kModify, kAll, kCount };
constexpr std::array<std::string_view, kCount> kCategoryNames{
    "passive limit", "aggressive limit", "market", "cancel", "modify", "ALL"};

struct Options {
    std::vector<std::string> engines{"reference", "map", "array", "array-scan"};
    std::vector<std::string> workloads{"realistic", "thin"};
    std::size_t ops{2'000'000};
    std::size_t warmup{200'000};
    int runs{5};
    int cpu{-1};
    std::uint64_t seed{42};
    std::string json;
};

struct Result {
    std::string engine;
    std::string workload;
    std::array<LatencyHistogram, kCount> latency{};
    std::vector<double> ns_per_op_runs;  // throughput runs
    std::size_t avg_live_orders{};
    std::size_t final_levels{};
};

std::vector<std::string> split_csv(const std::string& text) {
    std::vector<std::string> out;
    std::stringstream ss(text);
    for (std::string item; std::getline(ss, item, ',');) {
        out.push_back(item);
    }
    return out;
}

bool parse(int argc, char** argv, Options& o) {
    const std::vector<std::string_view> args(argv + 1, argv + argc);
    for (std::size_t i = 0; i < args.size(); ++i) {
        if (i + 1 >= args.size()) {
            return false;
        }
        const std::string_view flag = args[i];
        const std::string value{args[++i]};
        if (flag == "--engine") {
            o.engines = value == "all" ? Options{}.engines : split_csv(value);
        } else if (flag == "--workload") {
            o.workloads = value == "all" ? Options{}.workloads : split_csv(value);
        } else if (flag == "--ops") {
            o.ops = std::stoull(value);
        } else if (flag == "--warmup") {
            o.warmup = std::stoull(value);
        } else if (flag == "--runs") {
            o.runs = std::stoi(value);
        } else if (flag == "--cpu") {
            o.cpu = std::stoi(value);
        } else if (flag == "--seed") {
            o.seed = std::stoull(value);
        } else if (flag == "--json") {
            o.json = value;
        } else {
            return false;
        }
    }
    return o.runs > 0 && o.ops > 0;
}

Category classify(const lob::Command& command, bool traded) {
    if (const auto* order = std::get_if<lob::NewOrder>(&command)) {
        if (order->type == lob::OrderType::Market) {
            return kMarket;
        }
        return traded ? kAggressive : kPassive;
    }
    return std::holds_alternative<lob::Cancel>(command) ? kCancel : kModify;
}

template <class Book>
void measure(const lob::workload::Workload& w, const Options& o, Result& result) {
    const std::size_t warmup = std::min(o.warmup, w.commands.size());
    const auto& cmds = w.commands;

    for (int run = 0; run < o.runs; ++run) {
        // --- latency: one timestamp pair per command -----------------------
        {
            lob::CountingSink sink;
            auto book = std::make_unique<Book>(w.config.book, sink);
            for (std::size_t i = 0; i < warmup; ++i) {
                lob::process(*book, cmds[i]);
            }
            auto hist = std::make_unique<std::array<LatencyHistogram, kCount>>();
            for (std::size_t i = warmup; i < cmds.size(); ++i) {
                const std::uint64_t trades_before = sink.trades();
                const std::uint64_t t0 = lob::bench::TickClock::now();
                lob::process(*book, cmds[i]);
                const std::uint64_t t1 = lob::bench::TickClock::now();
                const Category category = classify(cmds[i], sink.trades() != trades_before);
                (*hist)[category].record(t1 - t0);
                (*hist)[kAll].record(t1 - t0);
            }
            for (std::size_t c = 0; c < kCount; ++c) {
                result.latency[c].merge((*hist)[c]);
            }
        }
        // --- throughput: the same commands timed as one block --------------
        {
            lob::CountingSink sink;
            auto book = std::make_unique<Book>(w.config.book, sink);
            for (std::size_t i = 0; i < warmup; ++i) {
                lob::process(*book, cmds[i]);
            }
            const auto start = std::chrono::steady_clock::now();
            for (std::size_t i = warmup; i < cmds.size(); ++i) {
                lob::process(*book, cmds[i]);
            }
            const auto stop = std::chrono::steady_clock::now();
            const double ns = std::chrono::duration<double, std::nano>(stop - start).count();
            result.ns_per_op_runs.push_back(ns / static_cast<double>(cmds.size() - warmup));
            if (sink.events() == 0) {
                std::cerr << "warning: no events emitted\n";
            }
        }
    }
}

void run_engine(const std::string& engine, const lob::workload::Workload& w, const Options& o,
                Result& r) {
    if (engine == "reference") {
        measure<lob::reference::ReferenceBook<lob::CountingSink>>(w, o, r);
    } else if (engine == "map") {
        measure<lob::MapEngine<lob::CountingSink>>(w, o, r);
    } else if (engine == "array") {
        measure<lob::ArrayEngine<lob::CountingSink>>(w, o, r);
    } else if (engine == "array-scan") {
        measure<lob::ArrayScanEngine<lob::CountingSink>>(w, o, r);
    } else {
        throw std::runtime_error("unknown engine: " + engine);
    }
}

double median(std::vector<double> v) {
    std::ranges::sort(v);
    return v.empty() ? 0.0 : v[v.size() / 2];
}

/// Typical (median) non-zero step the counter takes, in ticks: its effective
/// resolution. (The minimum is misleading: a counter scaled from a slower
/// clock can occasionally step by less than one underlying period.)
std::uint64_t timer_resolution_ticks() {
    LatencyHistogram steps;
    for (int i = 0; i < 200'000; ++i) {
        const std::uint64_t t0 = lob::bench::TickClock::now();
        std::uint64_t t1 = lob::bench::TickClock::now();
        while (t1 == t0) {
            t1 = lob::bench::TickClock::now();
        }
        steps.record(t1 - t0);
    }
    return steps.percentile(50);
}

LatencyHistogram timer_overhead() {
    LatencyHistogram h;
    for (int i = 0; i < 1'000'000; ++i) {
        const std::uint64_t t0 = lob::bench::TickClock::now();
        const std::uint64_t t1 = lob::bench::TickClock::now();
        h.record(t1 - t0);
    }
    return h;
}

constexpr std::array<double, 5> kPercentiles{50.0, 90.0, 99.0, 99.9, 100.0};
constexpr std::array<std::string_view, 5> kPercentileKeys{"p50", "p90", "p99", "p99_9", "max"};

void print_result(const Result& r, double ns_per_tick) {
    const double tput = median(r.ns_per_op_runs);
    std::printf("\n== %s / %s   (avg live %zu orders, %zu levels at end)\n", r.engine.c_str(),
                r.workload.c_str(), r.avg_live_orders, r.final_levels);
    std::printf("   throughput: %.1f ns/cmd median of %zu runs (%.1f M cmd/s)\n", tput,
                r.ns_per_op_runs.size(), 1e3 / tput);
    std::printf("   %-17s %10s %8s %8s %8s %8s %8s %9s   (ns)\n", "category", "count", "mean",
                "p50", "p90", "p99", "p99.9", "max");
    for (std::size_t c = 0; c < kCount; ++c) {
        const LatencyHistogram& h = r.latency[c];
        if (h.count() == 0) {
            continue;
        }
        std::printf("   %-17s %10llu %8.1f", std::string(kCategoryNames[c]).c_str(),
                    static_cast<unsigned long long>(h.count()), h.mean() * ns_per_tick);
        for (const double p : kPercentiles) {
            std::printf(p == 100.0 ? " %9.0f" : " %8.0f",
                        static_cast<double>(h.percentile(p)) * ns_per_tick);
        }
        std::printf("\n");
    }
}

void write_json(const std::string& path, const Options& o, const std::vector<Result>& results,
                double ns_per_tick, double resolution_ns, const LatencyHistogram& overhead,
                const std::string& placement) {
    std::ofstream out(path);
    out << "{\n  \"clock\": \"" << lob::bench::TickClock::name() << "\",\n"
        << "  \"ns_per_tick\": " << ns_per_tick << ",\n"
        << "  \"effective_resolution_ns\": " << resolution_ns << ",\n"
        << "  \"timer_overhead_ns\": {\"mean\": " << overhead.mean() * ns_per_tick
        << ", \"p50\": " << static_cast<double>(overhead.percentile(50)) * ns_per_tick
        << ", \"p99\": " << static_cast<double>(overhead.percentile(99)) * ns_per_tick << "},\n"
        << "  \"thread_placement\": \"" << placement << "\",\n"
        << "  \"ops\": " << o.ops << ", \"warmup\": " << o.warmup << ", \"runs\": " << o.runs
        << ", \"seed\": " << o.seed << ",\n  \"results\": [\n";
    for (std::size_t i = 0; i < results.size(); ++i) {
        const Result& r = results[i];
        out << "    {\"engine\": \"" << r.engine << "\", \"workload\": \"" << r.workload
            << "\", \"avg_live_orders\": " << r.avg_live_orders
            << ", \"final_levels\": " << r.final_levels
            << ", \"throughput_ns_per_op_median\": " << median(r.ns_per_op_runs)
            << ", \"throughput_ns_per_op_runs\": [";
        for (std::size_t k = 0; k < r.ns_per_op_runs.size(); ++k) {
            out << (k != 0 ? ", " : "") << r.ns_per_op_runs[k];
        }
        out << "],\n     \"latency_ns\": {";
        bool first = true;
        for (std::size_t c = 0; c < kCount; ++c) {
            const LatencyHistogram& h = r.latency[c];
            if (h.count() == 0) {
                continue;
            }
            out << (first ? "" : ", ") << "\"" << kCategoryNames[c]
                << "\": {\"count\": " << h.count() << ", \"mean\": " << h.mean() * ns_per_tick;
            first = false;
            for (std::size_t k = 0; k < kPercentiles.size(); ++k) {
                out << ", \"" << kPercentileKeys[k]
                    << "\": " << static_cast<double>(h.percentile(kPercentiles[k])) * ns_per_tick;
            }
            out << "}";
        }
        out << "}}" << (i + 1 < results.size() ? "," : "") << "\n";
    }
    out << "  ]\n}\n";
}

int run(const Options& o) {
    const std::string placement = lob::bench::place_thread(o.cpu);
    const double ns_per_tick = lob::bench::TickClock::ns_per_tick();
    const LatencyHistogram overhead = timer_overhead();
    const double resolution_ns = static_cast<double>(timer_resolution_ticks()) * ns_per_tick;

    std::printf("clock: %s, %.3f ns/tick, effective resolution %.1f ns\n",
                std::string(lob::bench::TickClock::name()).c_str(), ns_per_tick, resolution_ns);
    std::printf("timer overhead (back-to-back reads): mean %.1f ns, p50 %.1f ns, p99 %.1f ns\n",
                overhead.mean() * ns_per_tick,
                static_cast<double>(overhead.percentile(50)) * ns_per_tick,
                static_cast<double>(overhead.percentile(99)) * ns_per_tick);
    std::printf("thread: %s\n", placement.c_str());
    std::printf("commands: %zu timed + %zu warm-up per run, %d runs, seed %llu\n", o.ops, o.warmup,
                o.runs, static_cast<unsigned long long>(o.seed));

    std::vector<Result> results;
    for (const auto& workload_name : o.workloads) {
        const auto config = lob::workload::find_any_config(workload_name);
        if (!config) {
            std::cerr << "unknown workload: " << workload_name << '\n';
            return 2;
        }
        const auto w = lob::workload::make_workload(*config, o.seed, o.ops + o.warmup);
        for (const auto& engine : o.engines) {
            Result r;
            r.engine = engine;
            r.workload = workload_name;
            r.avg_live_orders = w.avg_live_orders;
            r.final_levels = w.final_levels;
            run_engine(engine, w, o, r);
            print_result(r, ns_per_tick);
            results.push_back(std::move(r));
        }
    }
    if (!o.json.empty()) {
        write_json(o.json, o, results, ns_per_tick, resolution_ns, overhead, placement);
        std::printf("\nwrote %s\n", o.json.c_str());
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        Options options;
        if (!parse(argc, argv, options)) {
            std::cerr << "usage: lob_latency [--engine reference,map,array,array-scan|all]\n"
                         "                   [--workload realistic,thin,replay,replay_sparse|all]\n"
                         "                   [--ops N] [--warmup N] [--runs N] [--cpu N]\n"
                         "                   [--seed N] [--json PATH]\n";
            return 2;
        }
        return run(options);
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << '\n';
        return 1;
    }
}
