#pragma once

#include "gateway/cache_key.h"

#include <atomic>
#include <cstddef>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <utility>

namespace replayarena {

// Thread-safe response cache, stage 1 of issue #2: get/set/erase with stats.
// Per-entry TTL (stage 2) and LRU eviction under a byte cap (stage 3) land as
// separate PRs; Stats already carries their fields so the API is stable.
//
// A hit returns the stored response byte-identical (a copy of the exact
// bytes that were set; no re-serialization). Readers share the lock; hit and
// miss counters are atomics because they mutate under the shared lock.
// Iteration order of the underlying map is never exposed, so it cannot leak
// into any recorded decision (SPEC.md section 6).
class ResponseCache {
 public:
  struct Stats {
    std::size_t hits = 0;
    std::size_t misses = 0;
    std::size_t evictions = 0; // stage 3; always 0 for now
    std::size_t entries = 0;
    std::size_t size_bytes = 0; // canonical key bytes + response bytes
  };

  [[nodiscard]] std::optional<std::string> get(const CacheKey& key) const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    const auto it = entries_.find(key);
    if (it == entries_.end()) {
      misses_.fetch_add(1, std::memory_order_relaxed);
      return std::nullopt;
    }
    hits_.fetch_add(1, std::memory_order_relaxed);
    return it->second;
  }

  // Inserts or overwrites.
  void set(const CacheKey& key, std::string response) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    const auto [it, inserted] = entries_.try_emplace(key);
    if (inserted) {
      size_bytes_ += key.bytes().size();
    } else {
      size_bytes_ -= it->second.size();
    }
    size_bytes_ += response.size();
    it->second = std::move(response);
  }

  // Returns true iff an entry was removed.
  bool erase(const CacheKey& key) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    const auto it = entries_.find(key);
    if (it == entries_.end()) {
      return false;
    }
    size_bytes_ -= key.bytes().size() + it->second.size();
    entries_.erase(it);
    return true;
  }

  [[nodiscard]] Stats stats() const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    Stats stats;
    stats.hits = hits_.load(std::memory_order_relaxed);
    stats.misses = misses_.load(std::memory_order_relaxed);
    stats.entries = entries_.size();
    stats.size_bytes = size_bytes_;
    return stats;
  }

 private:
  mutable std::shared_mutex mutex_;
  std::unordered_map<CacheKey, std::string, CacheKey::Hash> entries_;
  std::size_t size_bytes_ = 0; // mutated under exclusive lock only
  mutable std::atomic<std::size_t> hits_{0};
  mutable std::atomic<std::size_t> misses_{0};
};

} // namespace replayarena
