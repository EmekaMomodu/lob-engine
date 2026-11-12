// Differential tests: optimized engines vs. the reference book on random
// command streams. Every event and (periodically) the full book state must
// match.
//
// Size per case is LOB_DIFF_OPS (default 200'000); CI runs the default, and
// `tools/lob_fuzz` runs arbitrarily long campaigns locally.

#include "lob/workload/differential.hpp"

#include "lob/engines.hpp"
#include "lob/workload/generator.hpp"
#include "lob/workload/workload.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstdlib>
#include <sstream>
#include <string>
#include <tuple>
#include <vector>

namespace lob::workload {
namespace {

std::uint64_t ops_per_case() {
    // NOLINTNEXTLINE(concurrency-mt-unsafe): read once, single-threaded test setup
    if (const char* env = std::getenv("LOB_DIFF_OPS")) {
        return std::stoull(env);
    }
    return 200'000;
}

using Case = std::tuple<std::string, std::uint64_t>;  // config name, seed

class Differential : public ::testing::TestWithParam<Case> {
protected:
    template <template <class> class Engine>
    void run(const char* engine_name) {
        const auto& [name, seed] = GetParam();
        const auto config = find_config(name);
        ASSERT_TRUE(config.has_value()) << "unknown config " << name;
        const DiffOutcome outcome = run_differential<Engine>(
            *config, seed, DiffOptions{.ops = ops_per_case(), .engine = engine_name});
        ASSERT_TRUE(outcome.passed) << outcome.failure;
    }
};

TEST_P(Differential, MapEngineMatchesReference) {
    run<MapEngine>("map");
}
TEST_P(Differential, ArrayEngineMatchesReference) {
    run<ArrayEngine>("array");
}
TEST_P(Differential, ArrayScanEngineMatchesReference) {
    run<ArrayScanEngine>("array-scan");
}

INSTANTIATE_TEST_SUITE_P(Fuzz, Differential,
                         ::testing::Combine(::testing::Values("dense", "default", "edge", "deep"),
                                            ::testing::Values(1, 2, 3)),
                         [](const ::testing::TestParamInfo<Case>& param_info) {
                             return std::get<0>(param_info.param) + "_seed"
                                    + std::to_string(std::get<1>(param_info.param));
                         });

// --- The harness itself must catch bugs -------------------------------------

/// An engine with a planted bug: it wrongly rejects new orders whose id ends
/// in 7 (mod 1000) as duplicates.
template <class Sink>
class BuggyEngine : public MapEngine<Sink> {
public:
    BuggyEngine(const BookConfig& config, Sink& sink)
        : MapEngine<Sink>(config, sink), sink_(&sink) {}

    void submit(const NewOrder& cmd) {
        if (cmd.id.value() % 1000 == 7) {
            sink_->on(Rejected{.id = cmd.id, .reason = RejectReason::DuplicateOrderId});
            return;
        }
        MapEngine<Sink>::submit(cmd);
    }

private:
    Sink* sink_;
};

TEST(DifferentialHarness, ReportsPlantedBugWithReproduction) {
    const DiffOutcome outcome = run_differential<BuggyEngine>(
        default_config(), 1, DiffOptions{.ops = 100'000, .engine = "buggy"});
    ASSERT_FALSE(outcome.passed);
    EXPECT_NE(outcome.failure.find("EVENT MISMATCH"), std::string::npos) << outcome.failure;
    EXPECT_NE(outcome.failure.find("reproduce: lob_fuzz --engine buggy --config default --seed 1"),
              std::string::npos)
        << outcome.failure;
    EXPECT_LT(outcome.ops_run, 100'000U);
}

/// FNV-1a over the printed form of each command.
std::uint64_t fingerprint(const std::vector<Command>& commands) {
    std::uint64_t hash = 0xcbf29ce484222325ULL;
    std::ostringstream os;
    for (const Command& command : commands) {
        os.str("");
        os << command << '\n';
        for (const char ch : os.str()) {
            hash ^= static_cast<unsigned char>(ch);
            hash *= 0x100000001b3ULL;
        }
    }
    return hash;
}

// Pins the exact command stream for fixed seeds. CI runs this under GCC
// (libstdc++) and Clang/Apple Clang (libc++): if any platform consumes the RNG
// in a different order, or a generator change alters the stream, it fails.
// After an intentional generator change, update the constants (and note it).
TEST(DifferentialHarness, GeneratorGoldenSequence) {
    EXPECT_EQ(fingerprint(make_workload(dense_config(), 42, 20'000).commands),
              0xfe756d7bda205198ULL);
    EXPECT_EQ(fingerprint(make_workload(edge_config(), 7, 20'000).commands), 0xc5f48aaa0abc2040ULL);
}

TEST(DifferentialHarness, GeneratorIsDeterministic) {
    Generator a(dense_config(), 42);
    Generator b(dense_config(), 42);
    for (int i = 0; i < 10'000; ++i) {
        ASSERT_EQ(a.next(), b.next()) << "at command " << i;
    }
}

}  // namespace
}  // namespace lob::workload
