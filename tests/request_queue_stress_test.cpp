#include "gateway/cancel_token.h"
#include "gateway/request_queue.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <gtest/gtest.h>
#include <memory>
#include <optional>
#include <random>
#include <thread>
#include <vector>

// Concurrency tests for RequestQueue. These are the tests that must stay
// silent under TSan in CI; the ctest-level TIMEOUT property (see
// tests/CMakeLists.txt) is the deadlock watchdog. All PRNGs are fixed-seed:
// a failure here must reproduce.
namespace replayarena {
namespace {

using std::chrono::steady_clock;

steady_clock::time_point far_future() {
  return steady_clock::now() + std::chrono::minutes(5);
}

struct Item {
  std::size_t id;
};

// Conservation + exactly-once: every accepted request is delivered exactly
// once or was cancelled; every delivered request was accepted; nothing is
// delivered twice. 8 producers, ~30% of requests cancelled concurrently.
TEST(RequestQueueStress, ManyProducersWithCancellationConserveRequests) {
  constexpr std::size_t kProducers = 8;
  constexpr std::size_t kPerProducer = 200;
  constexpr std::size_t kTotal = kProducers * kPerProducer;

  RequestQueue<Item> queue(16);
  std::vector<CancelTokenPtr> tokens(kTotal);
  std::vector<std::atomic<bool>> accepted(kTotal);
  for (auto& t : tokens) {
    t = std::make_shared<CancelToken>();
  }

  std::vector<std::thread> producers;
  producers.reserve(kProducers);
  for (std::size_t p = 0; p < kProducers; ++p) {
    producers.emplace_back([&queue, &tokens, &accepted, p] {
      for (std::size_t i = 0; i < kPerProducer; ++i) {
        const std::size_t id = p * kPerProducer + i;
        if (queue.push(Item{id}, far_future(), tokens[id]) == PushResult::kOk) {
          accepted[id].store(true);
        }
      }
    });
  }

  // Cancel ~30% of all requests, racing the producers and the consumer.
  std::thread canceller([&tokens] {
    std::mt19937 rng(20260804);
    std::vector<std::size_t> ids(kTotal);
    for (std::size_t i = 0; i < kTotal; ++i) {
      ids[i] = i;
    }
    std::shuffle(ids.begin(), ids.end(), rng);
    for (std::size_t i = 0; i < kTotal * 3 / 10; ++i) {
      tokens[ids[i]]->cancel();
      if (i % 64 == 0) {
        std::this_thread::yield();
      }
    }
  });

  std::vector<std::size_t> delivered_count(kTotal, 0);
  std::thread consumer([&queue, &delivered_count] {
    while (auto item = queue.pop(far_future())) {
      ++delivered_count[item->id];
    }
  });

  for (auto& t : producers) {
    t.join();
  }
  canceller.join();
  queue.close();
  consumer.join();

  const auto stats = queue.stats();
  std::size_t delivered_total = 0;
  for (std::size_t id = 0; id < kTotal; ++id) {
    ASSERT_LE(delivered_count[id], 1u) << "request " << id << " delivered twice";
    ASSERT_TRUE(accepted[id].load()) << "far-future push failed for " << id;
    if (delivered_count[id] == 0) {
      // Never delivered: the only legal reason is cancellation.
      ASSERT_TRUE(tokens[id]->cancelled()) << "request " << id << " vanished";
    }
    delivered_total += delivered_count[id];
  }
  EXPECT_EQ(stats.pushed, kTotal);
  EXPECT_EQ(stats.popped, delivered_total);
  EXPECT_EQ(stats.popped + stats.skipped_cancelled, kTotal);
}

// close() must wake producers blocked mid-push; every producer gets a
// terminal result and nothing deadlocks (watchdog: ctest TIMEOUT).
TEST(RequestQueueStress, CloseWakesBlockedProducers) {
  constexpr std::size_t kProducers = 8;
  RequestQueue<int> queue(1);
  ASSERT_EQ(queue.try_push(0), PushResult::kOk); // make the queue full

  std::atomic<std::size_t> terminal_results{0};
  std::vector<std::thread> producers;
  producers.reserve(kProducers);
  for (std::size_t p = 0; p < kProducers; ++p) {
    producers.emplace_back([&queue, &terminal_results, p] {
      const PushResult result = queue.push(static_cast<int>(p) + 1, far_future());
      EXPECT_EQ(result, PushResult::kClosed);
      ++terminal_results;
    });
  }

  // Give producers a moment to block on the full queue, then close.
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  queue.close();
  for (auto& t : producers) {
    t.join();
  }
  EXPECT_EQ(terminal_results.load(), kProducers);

  // The pre-close item is still drainable.
  EXPECT_EQ(queue.pop(far_future()), std::optional<int>(0));
  EXPECT_EQ(queue.pop(far_future()), std::nullopt);
}

// Producers using short timeouts race a close at a random point; afterwards
// the books must balance: accepted == delivered + skipped-cancelled.
TEST(RequestQueueStress, RandomizedClosePointConservesAcceptedRequests) {
  for (std::uint32_t round = 0; round < 20; ++round) {
    RequestQueue<Item> queue(4);
    constexpr std::size_t kProducers = 4;
    constexpr std::size_t kPerProducer = 50;

    std::atomic<std::size_t> ok_count{0};
    std::vector<std::thread> producers;
    producers.reserve(kProducers);
    for (std::size_t p = 0; p < kProducers; ++p) {
      producers.emplace_back([&queue, &ok_count, p] {
        for (std::size_t i = 0; i < kPerProducer; ++i) {
          const auto deadline = steady_clock::now() + std::chrono::milliseconds(1);
          if (queue.push(Item{p * kPerProducer + i}, deadline) == PushResult::kOk) {
            ++ok_count;
          }
        }
      });
    }

    std::atomic<std::size_t> delivered{0};
    std::thread consumer([&queue, &delivered] {
      while (queue.pop(far_future())) {
        ++delivered;
      }
    });

    std::mt19937 rng(round); // per-round fixed seed
    std::this_thread::sleep_for(std::chrono::microseconds(static_cast<int>(rng() % 2000)));
    queue.close();

    for (auto& t : producers) {
      t.join();
    }
    consumer.join();

    const auto stats = queue.stats();
    EXPECT_EQ(stats.pushed, ok_count.load()) << "round " << round;
    EXPECT_EQ(stats.popped, delivered.load()) << "round " << round;
    EXPECT_EQ(stats.popped + stats.skipped_cancelled, stats.pushed) << "round " << round;
  }
}

} // namespace
} // namespace replayarena
