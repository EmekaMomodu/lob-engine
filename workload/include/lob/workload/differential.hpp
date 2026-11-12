#pragma once

// Differential test driver: run the reference book and an engine side by side
// on the same random command stream and stop at the first divergence.
//
// After every command the two event streams must be identical. Every
// `check_every` commands (and after the last one) the full order-level
// snapshots, best prices and order counts must match too, and the engine's
// own structural invariants must hold.

#include "lob/book_api.hpp"
#include "lob/commands.hpp"
#include "lob/io.hpp"
#include "lob/reference/reference_book.hpp"
#include "lob/sinks.hpp"
#include "lob/workload/generator.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <sstream>
#include <string>
#include <vector>

namespace lob::workload {

struct DiffOptions {
    std::uint64_t ops{100'000};
    std::uint64_t check_every{1'000};
    std::size_t history{24};  // commands included in a failure report
};

struct DiffOutcome {
    bool passed{true};
    std::uint64_t ops_run{0};
    std::string failure;  // human-readable report, empty if passed
};

namespace detail {

inline void print_events(std::ostream& os, const std::vector<Event>& events) {
    if (events.empty()) {
        os << "    (none)\n";
    }
    for (const Event& event : events) {
        os << "    " << event << '\n';
    }
}

inline std::string first_snapshot_difference(const BookSnapshot& ref, const BookSnapshot& eng) {
    std::ostringstream os;
    const auto compare_side = [&os](const char* name, const std::vector<LevelView>& a,
                                    const std::vector<LevelView>& b) {
        for (std::size_t i = 0; i < std::max(a.size(), b.size()); ++i) {
            if (i >= a.size() || i >= b.size() || !(a[i] == b[i])) {
                os << "  first difference on " << name << " at level index " << i << ":\n";
                os << "    reference: ";
                if (i < a.size()) {
                    os << a[i];
                } else {
                    os << "(no level)";
                }
                os << "\n    engine:    ";
                if (i < b.size()) {
                    os << b[i];
                } else {
                    os << "(no level)";
                }
                os << '\n';
                return true;
            }
        }
        return false;
    };
    if (!compare_side("bids", ref.bids, eng.bids)) {
        compare_side("asks", ref.asks, eng.asks);
    }
    return os.str();
}

}  // namespace detail

template <template <class> class Engine>
DiffOutcome run_differential(const GeneratorConfig& config, std::uint64_t seed,
                             const DiffOptions& options) {
    VectorSink ref_sink;
    VectorSink eng_sink;
    reference::ReferenceBook<VectorSink> ref(config.book, ref_sink);
    Engine<VectorSink> eng(config.book, eng_sink);
    Generator generator(config, seed);
    std::deque<Command> history;

    DiffOutcome outcome;
    const auto fail = [&](std::uint64_t op, const std::string& what, const std::string& detail) {
        std::ostringstream os;
        os << what << "\n  config=" << config.name << " seed=" << seed << " op=" << op
           << " (0-based)\n"
           << "  reproduce: lob_fuzz --config " << config.name << " --seed " << seed
           << " --ops " << (op + 1) << "\n"
           << detail << "  last " << history.size() << " commands (oldest first):\n";
        for (const Command& command : history) {
            os << "    " << command << '\n';
        }
        outcome.passed = false;
        outcome.ops_run = op + 1;
        outcome.failure = os.str();
        return outcome;
    };

    for (std::uint64_t op = 0; op < options.ops; ++op) {
        const Command command = generator.next();
        history.push_back(command);
        if (history.size() > options.history) {
            history.pop_front();
        }

        process(ref, command);
        process(eng, command);

        if (ref_sink.events() != eng_sink.events()) {
            std::ostringstream os;
            os << "  command: " << command << "\n  reference events:\n";
            detail::print_events(os, ref_sink.events());
            os << "  engine events:\n";
            detail::print_events(os, eng_sink.events());
            return fail(op, "EVENT MISMATCH", os.str());
        }
        for (const Event& event : ref_sink.events()) {
            generator.observe(event);
        }
        ref_sink.clear();
        eng_sink.clear();

        const bool last = op + 1 == options.ops;
        if (last || (op + 1) % options.check_every == 0) {
            const BookSnapshot ref_snap = ref.snapshot();
            const BookSnapshot eng_snap = eng.snapshot();
            if (!(ref_snap == eng_snap)) {
                return fail(op, "STATE MISMATCH",
                            detail::first_snapshot_difference(ref_snap, eng_snap));
            }
            if (ref.order_count() != eng.order_count() || ref.best_bid() != eng.best_bid() ||
                ref.best_ask() != eng.best_ask()) {
                return fail(op, "QUERY MISMATCH (best price or order count)", "");
            }
            if constexpr (requires { eng.check_invariants(); }) {
                if (const auto error = eng.check_invariants()) {
                    return fail(op, "ENGINE INVARIANT VIOLATED", "  " + *error + "\n");
                }
            }
        }
    }
    outcome.ops_run = options.ops;
    return outcome;
}

}  // namespace lob::workload
