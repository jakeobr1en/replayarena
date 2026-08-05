#pragma once

#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <optional>
#include <type_traits>
#include <utility>
#include <vector>

#include "gateway/cancel_token.h"

namespace replayarena {

// Result of a push attempt. No exceptions cross this module boundary; every
// outcome is an explicit value (SPEC.md section 7).
enum class PushResult {
  kOk,
  kQueueFull,  // try_push only: no capacity right now
  kTimeout,    // push only: capacity did not free up before the deadline
  kClosed,     // queue was closed; the request was not accepted
};

// Bounded multi-producer single-consumer queue with backpressure and
// cancellation (issue #1, SPEC.md section 4.1).
//
// - Capacity is fixed at construction; storage is pre-allocated and the hot
//   path never allocates.
// - Any thread may push; exactly one thread may call pop().
// - A request may carry a CancelToken. Requests cancelled before delivery are
//   never returned by pop(); their slot is reclaimed when the consumer skips
//   them. A request delivered by pop() counts as delivered even if its token
//   is cancelled afterwards; observing in-flight cancellation is the
//   consumer's responsibility.
// - close() wakes all blocked producers (they return kClosed) and the
//   consumer. pop() keeps draining accepted requests after close and returns
//   std::nullopt once the queue is empty.
//
// Terminal status contract: every request submitted with push/try_push gets
// exactly one terminal outcome. Push failures (kQueueFull, kTimeout, kClosed)
// are terminal at the call site; accepted requests (kOk) end either delivered
// by pop() or skipped-as-cancelled, never both.
template <typename T>
class RequestQueue {
  static_assert(std::is_move_constructible_v<T>, "RequestQueue requires a movable payload type");

 public:
  using Deadline = std::chrono::steady_clock::time_point;

  struct Stats {
    std::size_t pushed = 0;             // accepted by push/try_push
    std::size_t popped = 0;             // delivered to the consumer
    std::size_t skipped_cancelled = 0;  // reclaimed without delivery
  };

  // Precondition: capacity > 0.
  explicit RequestQueue(std::size_t capacity) : ring_(capacity), capacity_(capacity) {
    assert(capacity > 0);
  }

  RequestQueue(const RequestQueue&) = delete;
  RequestQueue& operator=(const RequestQueue&) = delete;

  // Non-blocking. kQueueFull if no slot is free right now.
  PushResult try_push(T value, CancelTokenPtr token = nullptr) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (closed_) {
      return PushResult::kClosed;
    }
    if (count_ == capacity_) {
      return PushResult::kQueueFull;
    }
    emplace_locked(std::move(value), std::move(token));
    return PushResult::kOk;
  }

  // Blocks until a slot frees, the deadline passes, or the queue closes.
  PushResult push(T value, Deadline deadline, CancelTokenPtr token = nullptr) {
    std::unique_lock<std::mutex> lock(mutex_);
    for (;;) {
      if (closed_) {
        return PushResult::kClosed;
      }
      if (count_ < capacity_) {
        emplace_locked(std::move(value), std::move(token));
        return PushResult::kOk;
      }
      if (not_full_.wait_until(lock, deadline) == std::cv_status::timeout) {
        if (closed_) {
          return PushResult::kClosed;
        }
        if (count_ < capacity_) {
          emplace_locked(std::move(value), std::move(token));
          return PushResult::kOk;
        }
        return PushResult::kTimeout;
      }
    }
  }

  // Single consumer only. Returns the oldest non-cancelled request, or
  // std::nullopt on deadline expiry or when the queue is closed and drained.
  std::optional<T> pop(Deadline deadline) {
    std::unique_lock<std::mutex> lock(mutex_);
    for (;;) {
      if (std::optional<T> value = take_front_locked()) {
        return value;
      }
      if (closed_) {
        return std::nullopt;
      }
      if (not_empty_.wait_until(lock, deadline) == std::cv_status::timeout) {
        return take_front_locked();  // last chance; may still be empty
      }
    }
  }

  // Idempotent. Wakes every blocked producer and the consumer.
  void close() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      closed_ = true;
    }
    not_full_.notify_all();
    not_empty_.notify_all();
  }

  [[nodiscard]] bool closed() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return closed_;
  }

  // Occupied slots, including cancelled entries not yet reclaimed.
  [[nodiscard]] std::size_t size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return count_;
  }

  [[nodiscard]] Stats stats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return stats_;
  }

 private:
  struct Slot {
    T value;
    CancelTokenPtr token;
  };

  void emplace_locked(T value, CancelTokenPtr token) {
    ring_[tail_].emplace(Slot{std::move(value), std::move(token)});
    tail_ = next(tail_);
    ++count_;
    ++stats_.pushed;
    not_empty_.notify_one();
  }

  // Drops cancelled entries at the head, then takes the head entry if one
  // remains. Frees slots for blocked producers in both cases.
  std::optional<T> take_front_locked() {
    std::size_t freed = 0;
    while (count_ > 0 && ring_[head_]->token != nullptr && ring_[head_]->token->cancelled()) {
      ring_[head_].reset();
      head_ = next(head_);
      --count_;
      ++stats_.skipped_cancelled;
      ++freed;
    }
    std::optional<T> value;
    if (count_ > 0) {
      value.emplace(std::move(ring_[head_]->value));
      ring_[head_].reset();
      head_ = next(head_);
      --count_;
      ++stats_.popped;
      ++freed;
    }
    if (freed == 1) {
      not_full_.notify_one();
    } else if (freed > 1) {
      not_full_.notify_all();
    }
    return value;
  }

  [[nodiscard]] std::size_t next(std::size_t index) const {
    return (index + 1 == capacity_) ? 0 : index + 1;
  }

  mutable std::mutex mutex_;
  std::condition_variable not_full_;
  std::condition_variable not_empty_;
  std::vector<std::optional<Slot>> ring_;
  const std::size_t capacity_;
  std::size_t head_ = 0;
  std::size_t tail_ = 0;
  std::size_t count_ = 0;
  bool closed_ = false;
  Stats stats_;
};

}  // namespace replayarena
