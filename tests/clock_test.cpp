#include "gateway/clock.h"

#include <chrono>
#include <gtest/gtest.h>
#include <thread>
#include <vector>

namespace replayarena {
namespace {

using namespace std::chrono_literals;

TEST(SimulatedClock, StartsAtGivenInstantAndOnlyMovesOnAdvance) {
  const Clock::TimePoint start{Clock::Duration{1000}};
  SimulatedClock clock(start);
  EXPECT_EQ(clock.now(), start);
  EXPECT_EQ(clock.now(), start); // no background progression
  clock.advance(5ms);
  EXPECT_EQ(clock.now(), start + 5ms);
}

TEST(SimulatedClock, DefaultStartIsEpochZero) {
  SimulatedClock clock;
  EXPECT_EQ(clock.now().time_since_epoch(), Clock::Duration::zero());
}

TEST(SimulatedClock, AdvancesAccumulate) {
  SimulatedClock clock;
  clock.advance(1ms);
  clock.advance(2ms);
  clock.advance(0ms); // zero step is legal and a no-op
  EXPECT_EQ(clock.now().time_since_epoch(), Clock::Duration(3ms));
}

TEST(SimulatedClock, ConcurrentAdvancesLoseNothing) {
  SimulatedClock clock;
  constexpr int kThreads = 8;
  constexpr int kStepsPerThread = 1000;
  std::vector<std::thread> threads;
  threads.reserve(kThreads);
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([&clock] {
      for (int i = 0; i < kStepsPerThread; ++i) {
        clock.advance(1us);
      }
    });
  }
  for (auto& t : threads) {
    t.join();
  }
  EXPECT_EQ(clock.now().time_since_epoch(), Clock::Duration(kThreads * kStepsPerThread * 1us));
}

TEST(SteadyClock, NowIsMonotonicNonDecreasing) {
  SteadyClock clock;
  Clock::TimePoint previous = clock.now();
  for (int i = 0; i < 1000; ++i) {
    const Clock::TimePoint current = clock.now();
    ASSERT_GE(current, previous);
    previous = current;
  }
}

TEST(Clock, PolymorphicUseThroughBaseReference) {
  SimulatedClock simulated;
  simulated.advance(7ms);
  const Clock& clock = simulated;
  EXPECT_EQ(clock.now().time_since_epoch(), Clock::Duration(7ms));
}

} // namespace
} // namespace replayarena
