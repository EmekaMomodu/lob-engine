// lob_fuzz: long-running differential fuzzing from the command line.
//
//   lob_fuzz --config dense --seed 1 --ops 10000000
//   lob_fuzz --config all --seeds 1-8 --ops 2000000
//   lob_fuzz --engine map --config default --seed 3 --ops 4568   # reproduce a failure
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
    std::string engine{"map"};
    std::string config{"all"};
    std::uint64_t seed_first{1};
    std::uint64_t seed_last{1};
    std::uint64_t ops{1'000'000};
    std::uint64_t check_every{10'000};
};

void usage() {
    std::cerr << "usage: lob_fuzz [--engine map] [--config dense|default|edge|deep|all]\n"
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
    return args.engine == "map" && args.seed_first <= args.seed_last && args.ops > 0;
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

    std::uint64_t total_ops = 0;
    const auto start = std::chrono::steady_clock::now();
    for (const auto& config : configs) {
        for (std::uint64_t seed = args.seed_first; seed <= args.seed_last; ++seed) {
            const auto t0 = std::chrono::steady_clock::now();
            const auto outcome = lob::workload::run_differential<lob::MapEngine>(
                config, seed,
                lob::workload::DiffOptions{.ops = args.ops, .check_every = args.check_every});
            const std::chrono::duration<double> dt = std::chrono::steady_clock::now() - t0;
            total_ops += outcome.ops_run;
            if (!outcome.passed) {
                std::cout << "FAIL " << args.engine << ' ' << config.name << " seed=" << seed
                          << '\n'
                          << outcome.failure;
                return 1;
            }
            std::cout << "ok   " << args.engine << ' ' << config.name << " seed=" << seed
                      << " ops=" << outcome.ops_run << " (" << dt.count() << " s)\n";
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
