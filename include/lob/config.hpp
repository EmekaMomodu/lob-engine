#pragma once

#include <cstddef>

namespace lob {

// Cache-line size used for alignment and layout decisions on hot structs.
//
// Apple Silicon reports 128-byte lines (`sysctl hw.cachelinesize`); x86-64
// uses 64. We deliberately do not use
// std::hardware_destructive_interference_size: GCC warns that its value is
// ABI-unstable (-Winterference-size), and libc++ only recently provides it.
// An explicit constant is easier to reason about and to document.
#if defined(__APPLE__) && defined(__aarch64__)
inline constexpr std::size_t kCacheLine = 128;
#else
inline constexpr std::size_t kCacheLine = 64;
#endif

static_assert((kCacheLine & (kCacheLine - 1)) == 0, "cache line size must be a power of two");

}  // namespace lob
