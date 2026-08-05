#include "gateway/cancel_token.h"
#include "gateway/request_queue.h"

#include <chrono>
#include <gtest/gtest.h>
#include <memory>
#include <optional>
#include <utility>

namespace replayarena {
namespace {

using std::chrono::steady_clock;

// A deadline that is already over: blocking calls must decide immediately.
steady_clock::time_point expired() {
  return steady_clock::now() - std::chrono::seconds(1);
}

// A deadline no correct test path should ever reach.
steady_clock::time_point far_future() {
  return steady_clock::now() + std::chrono::minutes(5);
}

TEST(RequestQueue, TryPushPopRoundtrip) {
  RequestQueue<int> queue(4);
  EXPECT_EQ(queue.try_push(7), PushResult::kOk);
  EXPECT_EQ(queue.pop(expired()), std::optional<int>(7));
}

TEST(RequestQueue, PopIsFifo) {
  RequestQueue<int> queue(4);
  ASSERT_EQ(queue.try_push(1), PushResult::kOk);
  ASSERT_EQ(queue.try_push(2), PushResult::kOk);
  ASSERT_EQ(queue.try_push(3), PushResult::kOk);
  EXPECT_EQ(queue.pop(expired()), std::optional<int>(1));
  EXPECT_EQ(queue.pop(expired()), std::optional<int>(2));
  EXPECT_EQ(queue.pop(expired()), std::optional<int>(3));
}

TEST(RequestQueue, TryPushOnFullFailsFast) {
  RequestQueue<int> queue(2);
  ASSERT_EQ(queue.try_push(1), PushResult::kOk);
  ASSERT_EQ(queue.try_push(2), PushResult::kOk);
  EXPECT_EQ(queue.try_push(3), PushResult::kQueueFull);
  EXPECT_EQ(queue.size(), 2u);
}

TEST(RequestQueue, PushOnFullTimesOut) {
  RequestQueue<int> queue(1);
  ASSERT_EQ(queue.try_push(1), PushResult::kOk);
  EXPECT_EQ(queue.push(2, expired()), PushResult::kTimeout);
}

TEST(RequestQueue, PopOnEmptyTimesOut) {
  RequestQueue<int> queue(1);
  EXPECT_EQ(queue.pop(expired()), std::nullopt);
}

TEST(RequestQueue, CancelledEntryIsSkippedAndFreesSlot) {
  RequestQueue<int> queue(2);
  auto doomed = std::make_shared<CancelToken>();
  ASSERT_EQ(queue.try_push(1, doomed), PushResult::kOk);
  ASSERT_EQ(queue.try_push(2), PushResult::kOk);
  ASSERT_EQ(queue.try_push(3), PushResult::kQueueFull);

  doomed->cancel();
  EXPECT_EQ(queue.pop(expired()), std::optional<int>(2));

  // Both the cancelled slot and the delivered slot are free again.
  EXPECT_EQ(queue.size(), 0u);
  EXPECT_EQ(queue.try_push(4), PushResult::kOk);
  EXPECT_EQ(queue.try_push(5), PushResult::kOk);

  const auto stats = queue.stats();
  EXPECT_EQ(stats.skipped_cancelled, 1u);
  EXPECT_EQ(stats.popped, 1u);
}

TEST(RequestQueue, QueueOfOnlyCancelledEntriesPopsEmpty) {
  RequestQueue<int> queue(2);
  auto a = std::make_shared<CancelToken>();
  auto b = std::make_shared<CancelToken>();
  ASSERT_EQ(queue.try_push(1, a), PushResult::kOk);
  ASSERT_EQ(queue.try_push(2, b), PushResult::kOk);
  a->cancel();
  b->cancel();
  EXPECT_EQ(queue.pop(expired()), std::nullopt);
  EXPECT_EQ(queue.size(), 0u);
  EXPECT_EQ(queue.stats().skipped_cancelled, 2u);
}

TEST(RequestQueue, TokenCancelledBeforePushIsNeverDelivered) {
  RequestQueue<int> queue(2);
  auto token = std::make_shared<CancelToken>();
  token->cancel();
  ASSERT_EQ(queue.try_push(1, token), PushResult::kOk);
  EXPECT_EQ(queue.pop(expired()), std::nullopt);
}

TEST(RequestQueue, PushAfterCloseIsRejected) {
  RequestQueue<int> queue(2);
  queue.close();
  EXPECT_EQ(queue.try_push(1), PushResult::kClosed);
  EXPECT_EQ(queue.push(2, far_future()), PushResult::kClosed);
}

TEST(RequestQueue, CloseDrainsAcceptedEntriesThenReturnsEmpty) {
  RequestQueue<int> queue(4);
  ASSERT_EQ(queue.try_push(1), PushResult::kOk);
  ASSERT_EQ(queue.try_push(2), PushResult::kOk);
  queue.close();
  EXPECT_EQ(queue.pop(far_future()), std::optional<int>(1));
  EXPECT_EQ(queue.pop(far_future()), std::optional<int>(2));
  // Closed and drained: returns immediately despite the far deadline.
  EXPECT_EQ(queue.pop(far_future()), std::nullopt);
}

TEST(RequestQueue, CloseIsIdempotent) {
  RequestQueue<int> queue(1);
  queue.close();
  queue.close();
  EXPECT_TRUE(queue.closed());
}

TEST(RequestQueue, MoveOnlyPayload) {
  RequestQueue<std::unique_ptr<int>> queue(1);
  ASSERT_EQ(queue.try_push(std::make_unique<int>(42)), PushResult::kOk);
  auto out = queue.pop(expired());
  ASSERT_TRUE(out.has_value());
  EXPECT_EQ(**out, 42);
}

TEST(RequestQueue, StatsCountAcceptedPushesOnly) {
  RequestQueue<int> queue(1);
  ASSERT_EQ(queue.try_push(1), PushResult::kOk);
  ASSERT_EQ(queue.try_push(2), PushResult::kQueueFull);
  ASSERT_EQ(queue.push(3, expired()), PushResult::kTimeout);
  EXPECT_EQ(queue.stats().pushed, 1u);
}

} // namespace
} // namespace replayarena
