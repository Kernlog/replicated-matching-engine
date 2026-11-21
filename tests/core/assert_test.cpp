#include "core/assert.hpp"

#include <gtest/gtest.h>

namespace {

TEST(AssertDeathTest, AbortsWithExpressionText) {
  const int value = 1;
  EXPECT_DEATH(RME_ASSERT(value == 2), "assertion failed: value == 2");
}

TEST(AssertTest, PassesSilentlyWhenTrue) {
  RME_ASSERT(1 + 1 == 2);
}

}  // namespace
