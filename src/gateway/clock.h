#pragma once

#include <atomic>
#include <cassert>
#include <chrono>

namespace replayarena {

// The gateway's only source of time.
//
// SPEC.md section 6 bans wall-clock reads as decision inputs: any component
// whose behavior depends on time (cache TTL, rate limiter refill, scheduler
// slack math) takes a Clock& and never calls a std::chrono clock directly.
// SteadyClock is the single place wall time enters the system; SimulatedClock
// makes every time-dependent behavior fully deterministic under test and
// replay. Grep discipline: std::chrono::steady_clock::now() may appear in
// this file and nowhere else under src/.
class Clock {
 public:
  using TimePoint = std::chrono::steady_clock::time_point;
  using Duration = std::chrono::steady_clock::duration;

  Clock() = default;
  Clock(const Clock&) = delete;
  Clock& operator=(const Clock&) = delete;
  virtual ~Clock() = default;

  // Monotonic: never decreases across calls on the same instance.
  [[nodiscard]] virtual TimePoint now() const = 0;
};

// Production clock backed by std::chrono::steady_clock.
class SteadyClock final : public Clock {
 public:
  [[nodiscard]] TimePoint now() const override { return std::chrono::steady_clock::now(); }
};

// Deterministic clock for tests and replay. Time moves only when advance()
// is called; there is no background progression, so time-dependent logic can
// be stepped to exact instants without sleeping. Thread-safe: any thread may
// call now() or advance().
class SimulatedClock final : public Clock {
 public:
  explicit SimulatedClock(TimePoint start = TimePoint{})
      : now_since_epoch_(start.time_since_epoch().count()) {}

  [[nodiscard]] TimePoint now() const override {
    return TimePoint(Duration(now_since_epoch_.load(std::memory_order_acquire)));
  }

  // Precondition: step >= 0 (a monotonic clock cannot go backwards).
  void advance(Duration step) {
    assert(step >= Duration::zero());
    now_since_epoch_.fetch_add(step.count(), std::memory_order_acq_rel);
  }

 private:
  std::atomic<Duration::rep> now_since_epoch_;
};

} // namespace replayarena
