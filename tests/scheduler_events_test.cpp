#include "gateway/scheduler_events.h"

#include <gtest/gtest.h>
#include <variant>
#include <vector>

namespace replayarena {
namespace {

// Event equality is load-bearing: replay (issue #4) decides divergence by
// comparing regenerated events against recorded ones. These tests pin the
// semantics equality must have.

TEST(SchedulerEvents, EqualEventsCompareEqual) {
  const SchedulerEvent a{5, BatchFormed{2, {10, 11, 12}}};
  const SchedulerEvent b{5, BatchFormed{2, {10, 11, 12}}};
  EXPECT_EQ(a, b);
}

TEST(SchedulerEvents, TickParticipatesInEquality) {
  const SchedulerEvent a{5, RequestCompleted{10}};
  const SchedulerEvent b{6, RequestCompleted{10}};
  EXPECT_NE(a, b);
}

TEST(SchedulerEvents, PayloadTypeParticipatesInEquality) {
  // Same tick, same id, different event kind: must differ.
  const SchedulerEvent a{5, RequestCompleted{10}};
  const SchedulerEvent b{5, RequestCancelled{10}};
  EXPECT_NE(a, b);
}

TEST(SchedulerEvents, BatchMemberOrderParticipatesInEquality) {
  // Dispatch order inside a batch is a scheduling decision; a reordered
  // batch is a divergence, not an equivalent batch.
  const SchedulerEvent a{5, BatchFormed{2, {10, 11}}};
  const SchedulerEvent b{5, BatchFormed{2, {11, 10}}};
  EXPECT_NE(a, b);
}

TEST(SchedulerEvents, RejectionReasonParticipatesInEquality) {
  const SchedulerEvent a{5, RequestRejected{10, RejectReason::kDeadlineInfeasible}};
  const SchedulerEvent b{5, RequestRejected{10, RejectReason::kQueueFull}};
  EXPECT_NE(a, b);
}

TEST(SchedulerEvents, NullSinkAcceptsEveryEventKind) {
  NullEventSink sink;
  SchedulerEventSink& as_interface = sink;
  const std::vector<SchedulerEvent> one_of_each{
      {1, RequestEnqueued{10, "client-a"}},
      {2, BatchFormed{1, {10}}},
      {3, BatchDispatched{1}},
      {4, BatchCompleted{1}},
      {5, RequestCompleted{10}},
      {6, RequestRejected{11, RejectReason::kShutdown}},
      {7, RequestCancelled{12}},
  };
  for (const auto& event : one_of_each) {
    as_interface.on_event(event); // must not throw, block, or mutate anything
  }
  SUCCEED();
}

} // namespace
} // namespace replayarena
