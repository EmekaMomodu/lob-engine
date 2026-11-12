#pragma once

// Low-overhead timestamps and thread placement for the latency harness.

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <thread>

#ifdef __x86_64__
#include <x86intrin.h>
#endif
#ifdef __linux__
#include <pthread.h>
#include <sched.h>
#endif
#ifdef __APPLE__
#include <pthread.h>
#include <sys/qos.h>
#endif

namespace lob::bench {

/// The fastest monotonic counter on this platform.
///
///   x86-64  RDTSC fenced by LFENCE. Invariant TSC assumed (all modern x86);
///           ticks are converted to ns by calibrating against steady_clock.
///   AArch64 CNTVCT_EL0 after an ISB. The frequency comes from CNTFRQ_EL0.
///           On Apple M4 / macOS 27 the counter reports 1 GHz units but
///           advances in ~41.7 ns steps (a 24 MHz source): coarser than a
///           typical order-book operation. The harness measures and prints
///           the effective step.
///   other   std::chrono::steady_clock.
class TickClock {
public:
    static std::uint64_t now() noexcept {
#ifdef __x86_64__
        _mm_lfence();
        const std::uint64_t t = __rdtsc();
        _mm_lfence();
        return t;
#elif defined(__aarch64__)
        std::uint64_t t = 0;
        // NOLINTNEXTLINE(portability-no-assembler): the counter is only readable via mrs
        asm volatile("isb\n\tmrs %0, cntvct_el0" : "=r"(t)::"memory");
        return t;
#else
        return static_cast<std::uint64_t>(
            std::chrono::steady_clock::now().time_since_epoch().count());
#endif
    }

    /// Nanoseconds per tick.
    static double ns_per_tick() {
#ifdef __aarch64__
        std::uint64_t frequency = 0;
        // NOLINTNEXTLINE(portability-no-assembler): the frequency is only readable via mrs
        asm volatile("mrs %0, cntfrq_el0" : "=r"(frequency));
        return 1e9 / static_cast<double>(frequency);
#elif defined(__x86_64__)
        using std::chrono::steady_clock;
        const auto wall0 = steady_clock::now();
        const std::uint64_t tick0 = now();
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        const auto wall1 = steady_clock::now();
        const std::uint64_t tick1 = now();
        const double ns = std::chrono::duration<double, std::nano>(wall1 - wall0).count();
        return ns / static_cast<double>(tick1 - tick0);
#else
        using period = std::chrono::steady_clock::period;
        return 1e9 * static_cast<double>(period::num) / static_cast<double>(period::den);
#endif
    }

    static std::string_view name() noexcept {
#ifdef __x86_64__
        return "rdtsc (lfence-fenced)";
#elif defined(__aarch64__)
        return "cntvct_el0 (isb-fenced)";
#else
        return "std::chrono::steady_clock";
#endif
    }
};

/// Pin the calling thread to `cpu` where the OS allows it. Returns a
/// human-readable description of what was actually applied.
// NOLINTNEXTLINE(modernize-use-string-view): the Linux branch builds the string
inline std::string place_thread(int cpu) {
#ifdef __linux__
    if (cpu < 0) {
        return "not pinned (pass --cpu N)";
    }
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(static_cast<std::size_t>(cpu), &set);
    if (pthread_setaffinity_np(pthread_self(), sizeof(set), &set) == 0) {
        return "pinned to CPU " + std::to_string(cpu);
    }
    return "pinning to CPU " + std::to_string(cpu) + " FAILED";
#elif defined(__APPLE__)
    (void)cpu;
    // macOS has no hard affinity on Apple Silicon. The strongest available
    // hint keeps the thread on performance cores.
    const bool ok = pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0) == 0;
    return ok ? "macOS: no hard affinity; QoS USER_INTERACTIVE (P-cores)"
              : "macOS: QoS request FAILED";
#else
    (void)cpu;
    return "no thread placement on this OS";
#endif
}

}  // namespace lob::bench
