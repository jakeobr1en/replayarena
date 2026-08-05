#include <gtest/gtest.h>

// Scaffold smoke test: proves the toolchain, GoogleTest integration, and
// CI sanitizer builds work end to end. Replaced by real tests from issue #1.
TEST(Scaffold, ToolchainIsAlive) {
  constexpr int kAnswer = 42;
  EXPECT_EQ(kAnswer, 42);
}
