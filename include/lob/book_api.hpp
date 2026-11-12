#pragma once

#include "lob/commands.hpp"
#include "lob/snapshot.hpp"
#include "lob/types.hpp"

#include <concepts>
#include <cstddef>
#include <optional>
#include <type_traits>
#include <variant>

namespace lob {

/// The interface every book implementation (reference, design A, design B)
/// satisfies. Tests are written once against this concept and instantiated
/// for each implementation.
template <class B>
concept OrderBook = requires(B& book, const B& cbook, const NewOrder& new_order,
                             const Cancel& cancel, const Modify& modify) {
    book.submit(new_order);
    book.cancel(cancel);
    book.modify(modify);
    { cbook.best_bid() } -> std::same_as<std::optional<Price>>;
    { cbook.best_ask() } -> std::same_as<std::optional<Price>>;
    { cbook.order_count() } -> std::same_as<std::size_t>;
    { cbook.snapshot() } -> std::same_as<BookSnapshot>;
};

/// Dispatch a type-erased command. Convenience for tests and replay tools;
/// benchmarks call submit/cancel/modify directly.
template <OrderBook B>
void apply(B& book, const Command& command) {
    std::visit(
        [&book](const auto& cmd) {
            using T = std::decay_t<decltype(cmd)>;
            if constexpr (std::is_same_v<T, NewOrder>) {
                book.submit(cmd);
            } else if constexpr (std::is_same_v<T, Cancel>) {
                book.cancel(cmd);
            } else {
                static_assert(std::is_same_v<T, Modify>);
                book.modify(cmd);
            }
        },
        command);
}

}  // namespace lob
