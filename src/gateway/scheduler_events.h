#pragma once

#include <cstdint>
#include <string>
#include <variant>
#include <vector>

namespace replayarena {

// The scheduler's decision stream (issue #3). Every scheduling decision is
// emitted as one event, stamped with the logical tick at which the single
// decision thread made it. The trace recorder (issue #4) implements the sink
// and persists these; replay compares regenerated events against recorded
// ones, which is why every event type defines equality.
//
// Ticks are logical, assigned by the scheduler, strictly increasing. Wall
// time never appears in an event (SPEC.md section 6).

enum class RejectReason : std::uint8_t {
  kDeadlineInfeasible, // could not meet the deadline even if dispatched now
  kQueueFull,
  kRateLimited, // rate limiter (lands with v0.2)
  kShutdown,
};

struct RequestEnqueued {
  std::uint64_t request_id = 0;
  std::string client_id;
  friend bool operator==(const RequestEnqueued&, const RequestEnqueued&) = default;
};

struct BatchFormed {
  std::uint64_t batch_id = 0;
  std::vector<std::uint64_t> request_ids; // in dispatch order
  friend bool operator==(const BatchFormed&, const BatchFormed&) = default;
};

struct BatchDispatched {
  std::uint64_t batch_id = 0;
  friend bool operator==(const BatchDispatched&, const BatchDispatched&) = default;
};

// Worker completions re-enter the scheduler as inputs; this event records
// the order in which the decision thread processed them, turning worker
// interleaving into replayable data.
struct BatchCompleted {
  std::uint64_t batch_id = 0;
  friend bool operator==(const BatchCompleted&, const BatchCompleted&) = default;
};

struct RequestCompleted {
  std::uint64_t request_id = 0;
  friend bool operator==(const RequestCompleted&, const RequestCompleted&) = default;
};

struct RequestRejected {
  std::uint64_t request_id = 0;
  RejectReason reason = RejectReason::kShutdown;
  friend bool operator==(const RequestRejected&, const RequestRejected&) = default;
};

struct RequestCancelled {
  std::uint64_t request_id = 0;
  friend bool operator==(const RequestCancelled&, const RequestCancelled&) = default;
};

using SchedulerEventPayload =
    std::variant<RequestEnqueued, BatchFormed, BatchDispatched, BatchCompleted, RequestCompleted,
                 RequestRejected, RequestCancelled>;

struct SchedulerEvent {
  std::uint64_t tick = 0;
  SchedulerEventPayload payload;
  friend bool operator==(const SchedulerEvent&, const SchedulerEvent&) = default;
};

// Consumes the decision stream. Called only from the scheduler's decision
// thread, in decision order; implementations need no internal ordering and
// must not block for long (they sit on the decision path).
class SchedulerEventSink {
 public:
  SchedulerEventSink() = default;
  SchedulerEventSink(const SchedulerEventSink&) = delete;
  SchedulerEventSink& operator=(const SchedulerEventSink&) = delete;
  virtual ~SchedulerEventSink() = default;

  virtual void on_event(const SchedulerEvent& event) = 0;
};

// Discards everything: the default until the trace recorder exists, and the
// baseline for measuring recording overhead (issue #4).
class NullEventSink final : public SchedulerEventSink {
 public:
  void on_event(const SchedulerEvent&) override {}
};

} // namespace replayarena
