#include "lob/config.hpp"

#include <gtest/gtest.h>

namespace {

TEST(Config, CacheLineIsPowerOfTwoAndAtLeast64) {
    EXPECT_GE(lob::kCacheLine, 64U);
    EXPECT_EQ(lob::kCacheLine & (lob::kCacheLine - 1), 0U);
}

}  // namespace
