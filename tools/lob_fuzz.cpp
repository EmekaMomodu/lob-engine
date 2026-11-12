// lob_fuzz: long-running differential fuzzing from the command line.
//
//   lob_fuzz --config dense --seed 1 --ops 10000000
//   lob_fuzz --config all --seeds 1-8 --ops 2000000
//   lob_fuzz --engine array --config default --seed 3 --ops 4568  # reproduce a failure
//
// Exit code 0 if every run matched the reference, 1 on the first mismatch
// (the full report is printed), 2 on bad arguments.

#include "lob/engines.hpp"
#include "lob/workload/differential.hpp"
#include "lob/workload/generator.hpp"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

struct Args {
    std::string engine{"all"};
    std::string config{"all"};
    std::uint64_t seed_first{1};
    std::uint64_t seed_last{1};
    std::uint64_t ops{1'000'000};
    std::uint64_t check_every{10'000};
};

void usage() {
    std::cerr << "usage: lob_fuzz [--engine map|array|array-scan|all]\n"
                 "                [--config dense|default|edge|deep|all]\n"
                 "                [--seed N | --seeds A-B] [--ops N] [--check-every N]\n";
}

bool parse(int argc, char** argv, Args& args) {
    const std::vector<std::string_view> argv_list(argv + 1, argv + argc);
    for (std::size_t i = 0; i < argv_list.size(); ++i) {
        const std::string_view flag = argv_list[i];
        if (i + 1 >= argv_list.size()) {
            return false;
        }
        const std::string value{argv_list[++i]};
        if (flag == "--engine") {
            args.engine = value;
        } else if (flag == "--config") {
            args.config = value;
        } else if (flag == "--seed") {
            args.seed_first = args.seed_last = std::stoull(value);
        } else if (flag == "--seeds") {
            const auto dash = value.find('-');
            if (dash == std::string::npos) {
                return false;
            }
            args.seed_first = std::stoull(value.substr(0, dash));
            args.seed_last = std::stoull(value.substr(dash + 1));
        } else if (flag == "--ops") {
            args.ops = std::stoull(value);
        } else if (flag == "--check-every") {
            args.check_every = std::stoull(value);
        } else {
            return false;
        }
    }
    const bool known_engine = args.engine == "map" || args.engine == "array"
                              || args.engine == "array-scan" || args.engine == "all";
    return known_engine && args.seed_first <= args.seed_last && args.ops > 0;
}

lob::workload::DiffOutcome run_engine(const std::string& engine,
                                      const lob::workload::GeneratorConfig& config,
                                      std::uint64_t seed,
                                      const lob::workload::DiffOptions& options) {
    if (engine == "map") {
        return lob::workload::run_differential<lob::MapEngine>(config, seed, options);
    }
    if (engine == "array") {
        return lob::workload::run_differential<lob::ArrayEngine>(config, seed, options);
    }
    return lob::workload::run_differential<lob::ArrayScanEngine>(config, seed, options);
}

int run(const Args& args) {
    std::vector<lob::workload::GeneratorConfig> configs;
    if (args.config == "all") {
        configs = lob::workload::named_configs();
    } else if (auto config = lob::workload::find_config(args.config)) {
        configs.push_back(*config);
    } else {
        std::cerr << "unknown config: " << args.config << '\n';
        return 2;
    }

    const std::vector<std::string> engines =
        args.engine == "all" ? std::vector<std::string>{"map", "array", "array-scan"}
                             : std::vector<std::string>{args.engine};

    std::uint64_t total_ops = 0;
    const auto start = std::chrono::steady_clock::now();
    for (const auto& engine : engines) {
        for (const auto& config : configs) {
            for (std::uint64_t seed = args.seed_first; seed <= args.seed_last; ++seed) {
                const auto t0 = std::chrono::steady_clock::now();
                const lob::workload::DiffOptions options{
                    .ops = args.ops, .check_every = args.check_every, .engine = engine.c_str()};
                const auto outcome = run_engine(engine, config, seed, options);
                const std::chrono::duration<double> dt = std::chrono::steady_clock::now() - t0;
                total_ops += outcome.ops_run;
                if (!outcome.passed) {
                    std::cout << "FAIL " << engine << ' ' << config.name << " seed=" << seed << '\n'
                              << outcome.failure;
                    return 1;
                }
                std::cout << "ok   " << engine << ' ' << config.name << " seed=" << seed
                          << " ops=" << outcome.ops_run << " (" << dt.count() << " s)\n";
            }
        }
    }
    const std::chrono::duration<double> elapsed = std::chrono::steady_clock::now() - start;
    std::cout << "PASS " << total_ops << " commands matched the reference in " << elapsed.count()
              << " s\n";
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        Args args;
        if (!parse(argc, argv, args)) {
            usage();
            return 2;
        }
        return run(args);
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << '\n';
        return 2;
    }
}
