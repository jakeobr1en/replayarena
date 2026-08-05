#pragma once

#include <atomic>
#include <memory>

namespace replayarena {

// Cooperative cancellation flag shared between a request's submitter and the
// components that process it. cancel() is idempotent and thread-safe.
class CancelToken {
 public:
  void cancel() noexcept { cancelled_.store(true, std::memory_order_release); }

  [[nodiscard]] bool cancelled() const noexcept {
    return cancelled_.load(std::memory_order_acquire);
  }

 private:
  std::atomic<bool> cancelled_{false};
};

using CancelTokenPtr = std::shared_ptr<CancelToken>;

}  // namespace replayarena
