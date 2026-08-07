#pragma once

#include "gateway/clock.h"
#include "gateway/request.h"
#include "gateway/scheduler_events.h"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace replayarena {

// Scheduling policy knobs (issue #3, SPEC.md section 4.3).
struct SchedulerConfig {
  std::size_t max_batch_size = 8;
  std::size_t max_inflight_batches = 1; // driver sets this to its worker count

  // Longest the head-of-line request may wait in the forming batch before
  // dispatch, deadline pressure aside. Zero means dispatch as soon as a
  // worker is free (no batching wait).
  Clock::Duration max_wait{};

  // Static estimate of how long a dispatched batch occupies a worker. Used
  // for feasibility (can this request still make its deadline?) and for the
  // early-dispatch trigger. Measured refinement is future work.
  Clock::Duration estimated_batch_latency{};

  // Safety margin subtracted from deadlines in the early-dispatch trigger.
  Clock::Duration slack_margin{};
};

enum class OutcomeStatus : std::uint8_t { kOk, kCancelled, kRejected };

// Terminal verdict on one request, delivered exactly once.
struct RequestOutcome {
  std::uint64_t request_id = 0;
  OutcomeStatus status = OutcomeStatus::kRejected;
  std::string output;                        // meaningful iff kOk
  std::optional<RejectReason> reject_reason; // engaged iff kRejected
  friend bool operator==(const RequestOutcome&, const RequestOutcome&) = default;
};

// Everything one input caused: batches to hand to workers, verdicts to
// deliver to submitters. Ordering within each vector is decision order.
struct SchedulerStep {
  std::vector<Batch> dispatches;
  std::vector<RequestOutcome> outcomes;
};

// The gateway's decision core (issue #3): continuous batching with
// earliest-deadline-first order and deadline-aware early dispatch.
//
// This class is deliberately pure machinery: no threads, no locks, no waits,
// no clock. Time is an argument. Feed it inputs (arrival, completion, time)
// and it returns what must happen; every decision is also emitted to the
// event sink with a strictly increasing logical tick. Given the same input
// sequence it produces byte-identical event and outcome sequences, which is
// what the replay harness (issue #4) checks. The threaded driver (worker
// pool, ingress queue) lives elsewhere and makes no decisions.
//
// Policy:
// - Pending requests order by (deadline, arrival order): EDF with a
//   deterministic tie-break.
// - A batch dispatches when a worker slot is free and any of: the pending
//   pool can fill a whole batch; the head request has waited max_wait; or
//   now + estimated_batch_latency + slack_margin >= head deadline (the last
//   safe moment to dispatch and still make it).
// - A request that cannot finish by its deadline even if dispatched now
//   (now + estimated_batch_latency > deadline) is rejected as infeasible at
//   arrival, or at formation time if it became infeasible while waiting.
// - Cancellation is observed at decision points (arrival and formation).
//   In-flight cancellation is v0.3 scope; a dispatched request runs to
//   completion in v0.2.
//
// Contract for callers (the driver): completions passed to on_completion
// must carry a batch id previously returned in a SchedulerStep dispatch and
// not yet completed. Request ids are unique per core lifetime (submitter
// contract, request.h).
class SchedulerCore {
 public:
  SchedulerCore(SchedulerConfig config, SchedulerEventSink& events)
      : config_(config), events_(events) {
    assert(config_.max_batch_size > 0);
    assert(config_.max_inflight_batches > 0);
  }

  SchedulerCore(const SchedulerCore&) = delete;
  SchedulerCore& operator=(const SchedulerCore&) = delete;

  [[nodiscard]] SchedulerStep on_arrival(Request request, Clock::TimePoint now) {
    SchedulerStep step;
    emit(RequestEnqueued{request.id, request.client_id});
    if (request.cancel_token != nullptr && request.cancel_token->cancelled()) {
      emit(RequestCancelled{request.id});
      step.outcomes.push_back(cancelled_outcome(request.id));
    } else if (infeasible(request, now)) {
      emit(RequestRejected{request.id, RejectReason::kDeadlineInfeasible});
      step.outcomes.push_back(rejected_outcome(request.id, RejectReason::kDeadlineInfeasible));
    } else {
      admit(std::move(request), now);
    }
    dispatch_ready(now, step);
    return step;
  }

  [[nodiscard]] SchedulerStep on_completion(std::uint64_t batch_id,
                                            std::vector<BackendResponse> responses,
                                            Clock::TimePoint now) {
    const auto it = std::find(inflight_batches_.begin(), inflight_batches_.end(), batch_id);
    assert(it != inflight_batches_.end());
    inflight_batches_.erase(it);

    SchedulerStep step;
    emit(BatchCompleted{batch_id});
    for (BackendResponse& response : responses) {
      emit(RequestCompleted{response.request_id});
      RequestOutcome outcome;
      outcome.request_id = response.request_id;
      outcome.status = OutcomeStatus::kOk;
      outcome.output = std::move(response.output);
      step.outcomes.push_back(std::move(outcome));
    }
    dispatch_ready(now, step);
    return step;
  }

  // Re-evaluates time-based triggers (max_wait, slack). The driver calls
  // this when a wakeup deadline passes without new input.
  [[nodiscard]] SchedulerStep on_time(Clock::TimePoint now) {
    SchedulerStep step;
    dispatch_ready(now, step);
    return step;
  }

  // The next instant a time-based trigger could fire, or nullopt if only new
  // input can change anything (idle, or all worker slots busy).
  [[nodiscard]] std::optional<Clock::TimePoint> next_wakeup(Clock::TimePoint now) const {
    if (pending_.empty() || inflight_batches_.size() >= config_.max_inflight_batches) {
      return std::nullopt;
    }
    if (pending_.size() >= config_.max_batch_size) {
      return now; // dispatchable immediately; driver should call on_time
    }
    const Pending& head = pending_.front();
    const Clock::TimePoint wait_expiry = head.admitted_at + config_.max_wait;
    const Clock::TimePoint last_safe_dispatch =
        head.request.deadline - config_.estimated_batch_latency - config_.slack_margin;
    return std::max(now, std::min(wait_expiry, last_safe_dispatch));
  }

  [[nodiscard]] std::size_t pending_count() const { return pending_.size(); }
  [[nodiscard]] std::size_t inflight_count() const { return inflight_batches_.size(); }

 private:
  struct Pending {
    Request request;
    Clock::TimePoint admitted_at;
    std::uint64_t arrival_seq = 0;
  };

  void emit(SchedulerEventPayload payload) {
    events_.on_event(SchedulerEvent{++tick_, std::move(payload)});
  }

  [[nodiscard]] bool infeasible(const Request& request, Clock::TimePoint now) const {
    return now + config_.estimated_batch_latency > request.deadline;
  }

  void admit(Request request, Clock::TimePoint now) {
    Pending pending{std::move(request), now, ++arrival_seq_};
    const auto pos = std::upper_bound(pending_.begin(), pending_.end(), pending, edf_order);
    pending_.insert(pos, std::move(pending));
  }

  // EDF with arrival order as the deterministic tie-break.
  static bool edf_order(const Pending& a, const Pending& b) {
    if (a.request.deadline != b.request.deadline) {
      return a.request.deadline < b.request.deadline;
    }
    return a.arrival_seq < b.arrival_seq;
  }

  [[nodiscard]] bool should_dispatch(Clock::TimePoint now) const {
    if (pending_.empty() || inflight_batches_.size() >= config_.max_inflight_batches) {
      return false;
    }
    if (pending_.size() >= config_.max_batch_size) {
      return true; // a full batch is waiting
    }
    const Pending& head = pending_.front();
    if (now - head.admitted_at >= config_.max_wait) {
      return true; // head has waited long enough
    }
    return now + config_.estimated_batch_latency + config_.slack_margin >= head.request.deadline;
  }

  // Dispatches as many batches as triggers and worker slots allow.
  void dispatch_ready(Clock::TimePoint now, SchedulerStep& step) {
    while (should_dispatch(now)) {
      form_batch(now, step);
    }
  }

  // Takes up to max_batch_size feasible requests from the EDF front. Entries
  // observed cancelled or infeasible here get their terminal verdict now and
  // never reach a worker.
  void form_batch(Clock::TimePoint now, SchedulerStep& step) {
    std::vector<Request> members;
    std::vector<std::uint64_t> member_ids;
    auto it = pending_.begin();
    while (it != pending_.end() && members.size() < config_.max_batch_size) {
      Request& request = it->request;
      if (request.cancel_token != nullptr && request.cancel_token->cancelled()) {
        emit(RequestCancelled{request.id});
        step.outcomes.push_back(cancelled_outcome(request.id));
      } else if (infeasible(request, now)) {
        emit(RequestRejected{request.id, RejectReason::kDeadlineInfeasible});
        step.outcomes.push_back(rejected_outcome(request.id, RejectReason::kDeadlineInfeasible));
      } else {
        member_ids.push_back(request.id);
        members.push_back(std::move(request));
      }
      it = pending_.erase(it);
    }
    if (members.empty()) {
      return; // everything at the front was cancelled or infeasible
    }
    Batch batch{++batch_seq_, std::move(members)};
    emit(BatchFormed{batch.id, std::move(member_ids)});
    emit(BatchDispatched{batch.id});
    inflight_batches_.push_back(batch.id);
    step.dispatches.push_back(std::move(batch));
  }

  static RequestOutcome cancelled_outcome(std::uint64_t request_id) {
    RequestOutcome outcome;
    outcome.request_id = request_id;
    outcome.status = OutcomeStatus::kCancelled;
    return outcome;
  }

  static RequestOutcome rejected_outcome(std::uint64_t request_id, RejectReason reason) {
    RequestOutcome outcome;
    outcome.request_id = request_id;
    outcome.status = OutcomeStatus::kRejected;
    outcome.reject_reason = reason;
    return outcome;
  }

  const SchedulerConfig config_;
  SchedulerEventSink& events_;
  std::vector<Pending> pending_; // sorted by edf_order
  std::vector<std::uint64_t> inflight_batches_;
  std::uint64_t tick_ = 0;
  std::uint64_t arrival_seq_ = 0;
  std::uint64_t batch_seq_ = 0;
};

} // namespace replayarena
