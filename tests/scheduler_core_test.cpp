#include "gateway/cancel_token.h"
#include "gateway/request.h"
#include "gateway/scheduler_core.h"
#include "gateway/scheduler_events.h"

#include <chrono>
#include <cstdint>
#include <gtest/gtest.h>
#include <memory>
#include <string>
#include <variant>
#include <vector>

namespace replayarena {
namespace {

using namespace std::chrono_literals;

constexpr Clock::TimePoint kBase = Clock::TimePoint{} + std::chrono::hours(1);

class RecordingSink final : public SchedulerEventSink {
 public:
  void on_event(const SchedulerEvent& event) override { events.push_back(event); }
  std::vector<SchedulerEvent> events;
};

Request req(std::uint64_t id, Clock::TimePoint deadline, CancelTokenPtr token = nullptr) {
  Request request;
  request.id = id;
  request.client_id = "client";
  request.model = "m";
  request.prompt = "p-" + std::to_string(id);
  request.deadline = deadline;
  request.cancel_token = std::move(token);
  return request;
}

BackendResponse resp(std::uint64_t id) {
  return BackendResponse{id, "out-" + std::to_string(id)};
}

template <typename E>
std::vector<E> events_of(const RecordingSink& sink) {
  std::vector<E> out;
  for (const auto& event : sink.events) {
    if (const E* e = std::get_if<E>(&event.payload)) {
      out.push_back(*e);
    }
  }
  return out;
}

// ---- batch formation triggers ----

TEST(SchedulerCore, FullBatchDispatchesImmediately) {
  RecordingSink sink;
  SchedulerConfig config;
  config.max_batch_size = 3;
  config.max_wait = 1h; // never triggers in this test
  SchedulerCore core(config, sink);

  EXPECT_TRUE(core.on_arrival(req(1, kBase + 1h), kBase).dispatches.empty());
  EXPECT_TRUE(core.on_arrival(req(2, kBase + 1h), kBase).dispatches.empty());
  const auto step = core.on_arrival(req(3, kBase + 1h), kBase);
  ASSERT_EQ(step.dispatches.size(), 1u);
  EXPECT_EQ(step.dispatches[0].requests.size(), 3u);
  EXPECT_EQ(core.pending_count(), 0u);
}

TEST(SchedulerCore, MaxWaitTriggersPartialDispatch) {
  RecordingSink sink;
  SchedulerConfig config;
  config.max_batch_size = 8;
  config.max_wait = 10ms;
  SchedulerCore core(config, sink);

  ASSERT_TRUE(core.on_arrival(req(1, kBase + 1h), kBase).dispatches.empty());
  EXPECT_TRUE(core.on_time(kBase + 9ms).dispatches.empty());
  const auto step = core.on_time(kBase + 10ms);
  ASSERT_EQ(step.dispatches.size(), 1u);
  EXPECT_EQ(step.dispatches[0].requests.size(), 1u);
}

TEST(SchedulerCore, HeadSlackTriggersEarlyDispatch) {
  RecordingSink sink;
  SchedulerConfig config;
  config.max_batch_size = 8;
  config.max_wait = 1h;
  config.estimated_batch_latency = 5ms;
  config.slack_margin = 1ms;
  SchedulerCore core(config, sink);

  // Deadline kBase+20ms: the last safe dispatch moment is 20-5-1 = +14ms.
  ASSERT_TRUE(core.on_arrival(req(1, kBase + 20ms), kBase).dispatches.empty());
  EXPECT_TRUE(core.on_time(kBase + 13ms).dispatches.empty());
  const auto step = core.on_time(kBase + 14ms);
  ASSERT_EQ(step.dispatches.size(), 1u);
}

TEST(SchedulerCore, ZeroMaxWaitMeansImmediateDispatch) {
  RecordingSink sink;
  SchedulerConfig config; // max_wait defaults to zero
  SchedulerCore core(config, sink);
  const auto step = core.on_arrival(req(1, kBase + 1h), kBase);
  ASSERT_EQ(step.dispatches.size(), 1u);
}

// ---- EDF ordering ----

TEST(SchedulerCore, BatchOrdersByDeadlineNotArrival) {
  RecordingSink sink;
  SchedulerConfig config;
  config.max_batch_size = 3;
  config.max_wait = 1h;
  SchedulerCore core(config, sink);

  ASSERT_TRUE(core.on_arrival(req(1, kBase + 30ms), kBase).dispatches.empty());
  ASSERT_TRUE(core.on_arrival(req(2, kBase + 10ms), kBase).dispatches.empty());
  const auto step = core.on_arrival(req(3, kBase + 20ms), kBase);
  ASSERT_EQ(step.dispatches.size(), 1u);

  const auto formed = events_of<BatchFormed>(sink);
  ASSERT_EQ(formed.size(), 1u);
  EXPECT_EQ(formed[0].request_ids, (std::vector<std::uint64_t>{2, 3, 1}));
}

TEST(SchedulerCore, EqualDeadlinesTieBreakByArrivalOrder) {
  RecordingSink sink;
  SchedulerConfig config;
  config.max_batch_size = 3;
  config.max_wait = 1h;
  SchedulerCore core(config, sink);

  ASSERT_TRUE(core.on_arrival(req(7, kBase + 10ms), kBase).dispatches.empty());
  ASSERT_TRUE(core.on_arrival(req(5, kBase + 10ms), kBase).dispatches.empty());
  (void)core.on_arrival(req(9, kBase + 10ms), kBase);

  const auto formed = events_of<BatchFormed>(sink);
  ASSERT_EQ(formed.size(), 1u);
  EXPECT_EQ(formed[0].request_ids, (std::vector<std::uint64_t>{7, 5, 9}));
}

// ---- feasibility ----

TEST(SchedulerCore, InfeasibleArrivalIsRejectedBeforeAnyWorker) {
  RecordingSink sink;
  SchedulerConfig config;
  config.estimated_batch_latency = 5ms;
  config.max_wait = 1h;
  SchedulerCore core(config, sink);

  const auto step = core.on_arrival(req(1, kBase + 3ms), kBase); // needs 5ms, has 3ms
  EXPECT_TRUE(step.dispatches.empty());
  ASSERT_EQ(step.outcomes.size(), 1u);
  EXPECT_EQ(step.outcomes[0].status, OutcomeStatus::kRejected);
  EXPECT_EQ(step.outcomes[0].reject_reason, RejectReason::kDeadlineInfeasible);

  const auto rejected = events_of<RequestRejected>(sink);
  ASSERT_EQ(rejected.size(), 1u);
  EXPECT_EQ(rejected[0].request_id, 1u);
}

TEST(SchedulerCore, ExactlyFeasibleArrivalIsAdmitted) {
  RecordingSink sink;
  SchedulerConfig config;
  config.estimated_batch_latency = 5ms;
  config.max_wait = 1h;
  SchedulerCore core(config, sink);

  // now + latency == deadline: still feasible, boundary included.
  const auto step = core.on_arrival(req(1, kBase + 5ms), kBase);
  EXPECT_TRUE(step.outcomes.empty());
  EXPECT_EQ(core.pending_count() + (step.dispatches.empty() ? 0u : 1u), 1u);
}

TEST(SchedulerCore, RequestBecomingInfeasibleWhileWaitingIsRejectedAtFormation) {
  RecordingSink sink;
  SchedulerConfig config;
  config.max_batch_size = 1;
  config.max_inflight_batches = 1;
  config.estimated_batch_latency = 5ms;
  SchedulerCore core(config, sink);

  // Occupy the only worker slot.
  const auto first = core.on_arrival(req(1, kBase + 1h), kBase);
  ASSERT_EQ(first.dispatches.size(), 1u);
  const std::uint64_t busy_batch = first.dispatches[0].id;

  // Feasible now (needs 5ms, has 8ms) but must wait for the worker.
  ASSERT_TRUE(core.on_arrival(req(2, kBase + 8ms), kBase).dispatches.empty());

  // Worker completes too late: request 2 can no longer make its deadline.
  const auto step = core.on_completion(busy_batch, {resp(1)}, kBase + 6ms);
  EXPECT_TRUE(step.dispatches.empty());
  ASSERT_EQ(step.outcomes.size(), 2u); // completion of 1, rejection of 2
  EXPECT_EQ(step.outcomes[0].status, OutcomeStatus::kOk);
  EXPECT_EQ(step.outcomes[1].request_id, 2u);
  EXPECT_EQ(step.outcomes[1].status, OutcomeStatus::kRejected);
  EXPECT_EQ(core.pending_count(), 0u);
}

// ---- cancellation ----

TEST(SchedulerCore, CancelledArrivalNeverAdmitted) {
  RecordingSink sink;
  SchedulerConfig config;
  SchedulerCore core(config, sink);

  auto token = std::make_shared<CancelToken>();
  token->cancel();
  const auto step = core.on_arrival(req(1, kBase + 1h, token), kBase);
  EXPECT_TRUE(step.dispatches.empty());
  ASSERT_EQ(step.outcomes.size(), 1u);
  EXPECT_EQ(step.outcomes[0].status, OutcomeStatus::kCancelled);
  EXPECT_EQ(core.pending_count(), 0u);
}

TEST(SchedulerCore, CancelledWhileWaitingIsSkippedAtFormationWithoutConsumingBatchSpace) {
  RecordingSink sink;
  SchedulerConfig config;
  config.max_batch_size = 2;
  config.max_inflight_batches = 1;
  SchedulerCore core(config, sink);

  const auto first = core.on_arrival(req(1, kBase + 1h), kBase);
  ASSERT_EQ(first.dispatches.size(), 1u);

  auto token = std::make_shared<CancelToken>();
  ASSERT_TRUE(core.on_arrival(req(2, kBase + 10min, token), kBase).dispatches.empty());
  ASSERT_TRUE(core.on_arrival(req(3, kBase + 20min), kBase).dispatches.empty());
  ASSERT_TRUE(core.on_arrival(req(4, kBase + 30min), kBase).dispatches.empty());
  token->cancel();

  const auto step = core.on_completion(first.dispatches[0].id, {resp(1)}, kBase + 1ms);
  ASSERT_EQ(step.dispatches.size(), 1u);
  // Request 2 was EDF-first but cancelled; the batch holds 3 and 4: the
  // cancelled entry did not consume batch capacity.
  const auto formed = events_of<BatchFormed>(sink);
  ASSERT_EQ(formed.size(), 2u);
  EXPECT_EQ(formed[1].request_ids, (std::vector<std::uint64_t>{3, 4}));

  const auto cancelled = events_of<RequestCancelled>(sink);
  ASSERT_EQ(cancelled.size(), 1u);
  EXPECT_EQ(cancelled[0].request_id, 2u);
}

// ---- worker slots ----

TEST(SchedulerCore, InflightCapBlocksDispatchUntilCompletion) {
  RecordingSink sink;
  SchedulerConfig config;
  config.max_batch_size = 1;
  config.max_inflight_batches = 1;
  SchedulerCore core(config, sink);

  const auto first = core.on_arrival(req(1, kBase + 1h), kBase);
  ASSERT_EQ(first.dispatches.size(), 1u);
  const auto second = core.on_arrival(req(2, kBase + 1h), kBase);
  EXPECT_TRUE(second.dispatches.empty());
  EXPECT_EQ(core.pending_count(), 1u);

  const auto step = core.on_completion(first.dispatches[0].id, {resp(1)}, kBase + 1ms);
  ASSERT_EQ(step.dispatches.size(), 1u);
  EXPECT_EQ(step.dispatches[0].requests[0].id, 2u);
}

TEST(SchedulerCore, CompletionDeliversOkOutcomesWithBackendOutput) {
  RecordingSink sink;
  SchedulerConfig config;
  config.max_batch_size = 2;
  config.max_wait = 1h; // accumulate to a full batch
  SchedulerCore core(config, sink);

  (void)core.on_arrival(req(1, kBase + 1h), kBase);
  const auto dispatched = core.on_arrival(req(2, kBase + 1h), kBase);
  ASSERT_EQ(dispatched.dispatches.size(), 1u);

  const auto step =
      core.on_completion(dispatched.dispatches[0].id, {resp(1), resp(2)}, kBase + 2ms);
  ASSERT_EQ(step.outcomes.size(), 2u);
  EXPECT_EQ(step.outcomes[0], (RequestOutcome{1, OutcomeStatus::kOk, "out-1", {}}));
  EXPECT_EQ(step.outcomes[1], (RequestOutcome{2, OutcomeStatus::kOk, "out-2", {}}));

  const auto completed = events_of<RequestCompleted>(sink);
  ASSERT_EQ(completed.size(), 2u);
  ASSERT_EQ(events_of<BatchCompleted>(sink).size(), 1u);
}

// ---- next_wakeup ----

TEST(SchedulerCore, NextWakeupIsNulloptWhenIdle) {
  RecordingSink sink;
  SchedulerConfig config;
  SchedulerCore core(config, sink);
  EXPECT_EQ(core.next_wakeup(kBase), std::nullopt);
}

TEST(SchedulerCore, NextWakeupIsNulloptWhenAllWorkersBusy) {
  RecordingSink sink;
  SchedulerConfig config;
  config.max_batch_size = 1;
  config.max_inflight_batches = 1;
  SchedulerCore core(config, sink);
  ASSERT_EQ(core.on_arrival(req(1, kBase + 1h), kBase).dispatches.size(), 1u);
  ASSERT_TRUE(core.on_arrival(req(2, kBase + 1h), kBase).dispatches.empty());
  // Pending work exists, but only a completion (new input) can unblock it.
  EXPECT_EQ(core.next_wakeup(kBase), std::nullopt);
}

TEST(SchedulerCore, NextWakeupIsMinOfMaxWaitExpiryAndLastSafeDispatch) {
  RecordingSink sink;
  SchedulerConfig config;
  config.max_batch_size = 8;
  config.max_wait = 10ms;
  config.estimated_batch_latency = 5ms;
  config.slack_margin = 1ms;
  SchedulerCore core(config, sink);

  // Deadline far away: max_wait expiry (kBase+10ms) is the earlier trigger.
  ASSERT_TRUE(core.on_arrival(req(1, kBase + 1h), kBase).dispatches.empty());
  EXPECT_EQ(core.next_wakeup(kBase), kBase + 10ms);

  // A tighter-deadline head changes the trigger: last safe dispatch for a
  // kBase+8ms deadline is 8-5-1 = +2ms, earlier than any wait expiry.
  ASSERT_TRUE(core.on_arrival(req(2, kBase + 8ms), kBase).dispatches.empty());
  EXPECT_EQ(core.next_wakeup(kBase), kBase + 2ms);
}

TEST(SchedulerCore, NextWakeupClampsToNowWhenTriggerIsAlreadyPast) {
  RecordingSink sink;
  SchedulerConfig config;
  config.max_batch_size = 8;
  config.max_wait = 10ms;
  SchedulerCore core(config, sink);

  ASSERT_TRUE(core.on_arrival(req(1, kBase + 1h), kBase).dispatches.empty());
  // The head's wait expired at kBase+10ms but on_time was never called;
  // the wakeup must be now, never an instant in the past.
  EXPECT_EQ(core.next_wakeup(kBase + 50ms), kBase + 50ms);
}

// ---- ticks and determinism ----

TEST(SchedulerCore, TicksStrictlyIncreaseAcrossAllEvents) {
  RecordingSink sink;
  SchedulerConfig config;
  config.max_batch_size = 2;
  config.max_wait = 1h; // accumulate to a full batch
  SchedulerCore core(config, sink);

  (void)core.on_arrival(req(1, kBase + 1h), kBase);
  const auto d = core.on_arrival(req(2, kBase + 1h), kBase);
  ASSERT_EQ(d.dispatches.size(), 1u);
  (void)core.on_completion(d.dispatches[0].id, {resp(1), resp(2)}, kBase + 1ms);

  ASSERT_GE(sink.events.size(), 2u);
  for (std::size_t i = 1; i < sink.events.size(); ++i) {
    EXPECT_LT(sink.events[i - 1].tick, sink.events[i].tick);
  }
}

TEST(SchedulerCore, IdenticalInputSequencesProduceIdenticalEventAndOutcomeStreams) {
  const auto drive = [](RecordingSink& sink, std::vector<RequestOutcome>& outcomes) {
    SchedulerConfig config;
    config.max_batch_size = 2;
    config.max_inflight_batches = 1;
    config.max_wait = 10ms;
    config.estimated_batch_latency = 5ms;
    SchedulerCore core(config, sink);
    std::vector<Batch> dispatched;
    const auto collect = [&outcomes, &dispatched](SchedulerStep step) {
      outcomes.insert(outcomes.end(), step.outcomes.begin(), step.outcomes.end());
      for (Batch& batch : step.dispatches) {
        dispatched.push_back(std::move(batch));
      }
    };

    auto token = std::make_shared<CancelToken>();
    collect(core.on_arrival(req(1, kBase + 30ms), kBase));
    collect(core.on_arrival(req(2, kBase + 25ms), kBase + 1ms));
    collect(core.on_arrival(req(3, kBase + 7ms), kBase + 2ms)); // boundary-feasible
    collect(core.on_arrival(req(4, kBase + 40ms, token), kBase + 3ms));
    token->cancel();
    // Drain every dispatch, including ones caused by completions (FIFO).
    std::size_t next = 0;
    Clock::TimePoint now = kBase + 6ms;
    while (next < dispatched.size()) {
      std::vector<BackendResponse> responses;
      for (const auto& r : dispatched[next].requests) {
        responses.push_back(resp(r.id));
      }
      collect(core.on_completion(dispatched[next].id, std::move(responses), now));
      ++next;
      now += 2ms;
    }
    collect(core.on_time(kBase + 50ms));
  };

  RecordingSink sink_a;
  RecordingSink sink_b;
  std::vector<RequestOutcome> outcomes_a;
  std::vector<RequestOutcome> outcomes_b;
  drive(sink_a, outcomes_a);
  drive(sink_b, outcomes_b);

  EXPECT_EQ(sink_a.events, sink_b.events);
  EXPECT_EQ(outcomes_a, outcomes_b);
  EXPECT_FALSE(sink_a.events.empty());
}

// ---- terminal-status conservation ----

TEST(SchedulerCore, EveryRequestGetsExactlyOneTerminalOutcome) {
  RecordingSink sink;
  SchedulerConfig config;
  config.max_batch_size = 2;
  config.max_inflight_batches = 2;
  config.max_wait = 5ms;
  config.estimated_batch_latency = 2ms;
  SchedulerCore core(config, sink);

  std::vector<RequestOutcome> outcomes;
  std::vector<Batch> to_complete;
  const auto absorb = [&](const SchedulerStep& step) {
    outcomes.insert(outcomes.end(), step.outcomes.begin(), step.outcomes.end());
    to_complete.insert(to_complete.end(), step.dispatches.begin(), step.dispatches.end());
  };

  auto cancel_me = std::make_shared<CancelToken>();
  absorb(core.on_arrival(req(1, kBase + 1h), kBase));
  absorb(core.on_arrival(req(2, kBase + 1ms), kBase)); // infeasible (needs 2ms)
  absorb(core.on_arrival(req(3, kBase + 1h, cancel_me), kBase));
  cancel_me->cancel();
  absorb(core.on_arrival(req(4, kBase + 1h), kBase));
  absorb(core.on_time(kBase + 5ms));
  while (!to_complete.empty()) {
    Batch batch = std::move(to_complete.back());
    to_complete.pop_back();
    std::vector<BackendResponse> responses;
    for (const auto& r : batch.requests) {
      responses.push_back(resp(r.id));
    }
    absorb(core.on_completion(batch.id, std::move(responses), kBase + 10ms));
  }

  ASSERT_EQ(outcomes.size(), 4u);
  std::vector<bool> seen(5, false);
  for (const auto& outcome : outcomes) {
    ASSERT_FALSE(seen[outcome.request_id]) << "duplicate outcome for " << outcome.request_id;
    seen[outcome.request_id] = true;
  }
  EXPECT_EQ(core.pending_count(), 0u);
  EXPECT_EQ(core.inflight_count(), 0u);
}

} // namespace
} // namespace replayarena
