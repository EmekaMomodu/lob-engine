// Verifies the "no heap allocation on the hot path" requirement directly.
//
// Allocations are counted by replacing the global operator new/delete (normal
// builds) or through AddressSanitizer's malloc hooks (ASan builds). A workload is pre-generated
// (allocating freely), the engine is constructed (it allocates all of its memory up front), and
// then counting is switched on for the replay. Any allocation by the engine while processing
// commands fails the test.

#include "lob/book_api.hpp"
#include "lob/engines.hpp"
#include "lob/sinks.hpp"
#include "lob/workload/workload.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <memory>
#include <new>

#ifdef __SANITIZE_ADDRESS__
#define LOB_ASAN_BUILD 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define LOB_ASAN_BUILD 1
#endif
#endif

namespace {

std::atomic<bool> g_counting{false};
std::atomic<std::size_t> g_allocations{0};

void note_allocation() noexcept {
    if (g_counting.load(std::memory_order_relaxed)) {
        g_allocations.fetch_add(1, std::memory_order_relaxed);
    }
}

class AllocationCounter {
public:
    AllocationCounter() {
        g_allocations.store(0);
        g_counting.store(true);
    }
    AllocationCounter(const AllocationCounter&) = delete;
    AllocationCounter& operator=(const AllocationCounter&) = delete;
    AllocationCounter(AllocationCounter&&) = delete;
    AllocationCounter& operator=(AllocationCounter&&) = delete;
    ~AllocationCounter() { g_counting.store(false); }

    [[nodiscard]] static std::size_t count() { return g_allocations.load(); }
};

}  // namespace

#ifdef LOB_ASAN_BUILD
// Under AddressSanitizer the runtime owns global operator new/delete; replacing
// them as well mixes two allocators (alloc-dealloc-mismatch on Linux). ASan
// offers malloc/free hooks instead, which see every heap allocation including
// operator new.
//
// Declared here rather than via <sanitizer/allocator_interface.h>, which GCC
// does not install (its ASan runtime still exports the function).
// NOLINTNEXTLINE(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp)
extern "C" int __sanitizer_install_malloc_and_free_hooks(void (*malloc_hook)(const volatile void*,
                                                                             std::size_t),
                                                         void (*free_hook)(const volatile void*));

namespace {
void on_malloc(const volatile void* /*ptr*/, std::size_t /*size*/) {
    note_allocation();
}
void on_free(const volatile void* /*ptr*/) {}
[[maybe_unused]] const int g_hooks_installed =
    __sanitizer_install_malloc_and_free_hooks(on_malloc, on_free);
}  // namespace

#else
// Without sanitizers: replace the global allocation functions with counting,
// malloc-based versions.

#if defined(__GNUC__) && !defined(__clang__)
// GCC inlines the replacement operator delete below into callers and then
// flags free() of memory that came from operator new, not seeing that our
// operator new is malloc-based. The pairing is correct by construction.
#pragma GCC diagnostic ignored "-Wmismatched-new-delete"
#endif

namespace {

void* counted_alloc(std::size_t size, std::size_t align) {
    note_allocation();
    if (size == 0) {
        size = 1;
    }
    void* ptr = nullptr;
    if (align <= alignof(std::max_align_t)) {
        ptr = std::malloc(size);  // NOLINT(cppcoreguidelines-no-malloc)
    } else {
        const std::size_t rounded = (size + align - 1) / align * align;
        ptr = std::aligned_alloc(align, rounded);  // NOLINT(cppcoreguidelines-no-malloc)
    }
    if (ptr == nullptr) {
        throw std::bad_alloc();
    }
    return ptr;
}

void counted_free(void* ptr) noexcept {
    std::free(ptr);  // NOLINT(cppcoreguidelines-no-malloc,cppcoreguidelines-owning-memory)
}

}  // namespace

// NOLINTBEGIN(cppcoreguidelines-owning-memory,misc-new-delete-overloads)
void* operator new(std::size_t size) {
    return counted_alloc(size, alignof(std::max_align_t));
}
void* operator new[](std::size_t size) {
    return counted_alloc(size, alignof(std::max_align_t));
}
void* operator new(std::size_t size, std::align_val_t align) {
    return counted_alloc(size, static_cast<std::size_t>(align));
}
void* operator new[](std::size_t size, std::align_val_t align) {
    return counted_alloc(size, static_cast<std::size_t>(align));
}
void operator delete(void* ptr) noexcept {
    counted_free(ptr);
}
void operator delete[](void* ptr) noexcept {
    counted_free(ptr);
}
void operator delete(void* ptr, std::size_t /*size*/) noexcept {
    counted_free(ptr);
}
void operator delete[](void* ptr, std::size_t /*size*/) noexcept {
    counted_free(ptr);
}
void operator delete(void* ptr, std::align_val_t /*align*/) noexcept {
    counted_free(ptr);
}
void operator delete[](void* ptr, std::align_val_t /*align*/) noexcept {
    counted_free(ptr);
}
void operator delete(void* ptr, std::size_t /*size*/, std::align_val_t /*align*/) noexcept {
    counted_free(ptr);
}
void operator delete[](void* ptr, std::size_t /*size*/, std::align_val_t /*align*/) noexcept {
    counted_free(ptr);
}
// NOLINTEND(cppcoreguidelines-owning-memory,misc-new-delete-overloads)
#endif  // LOB_ASAN_BUILD

namespace lob {
namespace {

TEST(NoAllocation, CounterDetectsAllocations) {
    const AllocationCounter counter;
    const auto probe = std::make_unique<int>(42);
    EXPECT_GE(AllocationCounter::count(), 1U);
    EXPECT_EQ(*probe, 42);
}

template <template <class> class Engine>
void expect_no_allocations(const workload::GeneratorConfig& config, std::size_t ops) {
    const workload::Workload w = workload::make_workload(config, 7, ops);

    CountingSink sink;
    const auto book = std::make_unique<Engine<CountingSink>>(config.book, sink);

    std::size_t allocations = 0;
    {
        const AllocationCounter counter;
        for (const Command& command : w.commands) {
            process(*book, command);
        }
        allocations = AllocationCounter::count();
    }
    EXPECT_EQ(allocations, 0U) << "engine allocated while processing " << ops << " commands ("
                               << config.name << ")";
    EXPECT_GT(sink.trades(), 0U) << "workload should exercise matching";
}

// Every named workload: covers rejects, sweeps, modifies, level create/erase,
// capacity exhaustion and id reuse, all of which must stay allocation-free.
template <template <class> class Engine>
void expect_no_allocations_all_workloads() {
    for (const auto& config : workload::named_configs()) {
        expect_no_allocations<Engine>(config, 200'000);
    }
    expect_no_allocations<Engine>(workload::replay_config(), 200'000);
}

TEST(NoAllocation, MapEngine) {
    expect_no_allocations_all_workloads<MapEngine>();
}
TEST(NoAllocation, ArrayEngine) {
    expect_no_allocations_all_workloads<ArrayEngine>();
}
TEST(NoAllocation, ArrayScanEngine) {
    expect_no_allocations_all_workloads<ArrayScanEngine>();
}

}  // namespace
}  // namespace lob
