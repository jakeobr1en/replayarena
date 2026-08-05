#pragma once

#include "gateway/cache_key.h"
#include "gateway/clock.h"

#include <atomic>
#include <cstddef>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <utility>

namespace replayarena {

// Thread-safe response cache, stages 1+2 of issue #2: get/set/erase plus
// per-entry TTL. LRU eviction under a byte cap (stage 3) lands as a separate
// PR; Stats already carries its fields so the API is stable.
//
// Time: all expiry decisions are evaluated against the injected Clock, never
// std::chrono directly, so expiry is fully deterministic under test and
// replay (SPEC.md section 6). An entry set at time t with TTL d is expired
// once now() >= t + d: visible strictly before the boundary, gone at it.
// Expired entries are never returned; they are reclaimed lazily when a get
// observes them and in bulk by sweep().
//
// A hit returns the stored response byte-identical (a copy of the exact
// bytes that were set; no re-serialization). Readers share the lock; hit and
// miss counters are atomics because they mutate under the shared lock.
// Iteration order of the underlying map is never exposed (sweep returns only
// a count), so it cannot leak into any recorded decision.
class ResponseCache {
 public:
  struct Stats {
    std::size_t hits = 0;
    std::size_t misses = 0;
    std::size_t expirations = 0; // entries reclaimed after TTL expiry
    std::size_t evictions = 0;   // stage 3; always 0 for now
    std::size_t entries = 0;     // live entries, including not-yet-reclaimed expired ones
    std::size_t size_bytes = 0;  // canonical key bytes + response bytes
  };

  explicit ResponseCache(const Clock& clock) : clock_(clock) {}

  // An expired entry counts as a miss and is reclaimed before returning.
  [[nodiscard]] std::optional<std::string> get(const CacheKey& key) {
    {
      std::shared_lock<std::shared_mutex> lock(mutex_);
      const auto it = entries_.find(key);
      if (it == entries_.end()) {
        misses_.fetch_add(1, std::memory_order_relaxed);
        return std::nullopt;
      }
      if (!expired(it->second)) {
        hits_.fetch_add(1, std::memory_order_relaxed);
        return it->second.response;
      }
    }
    reclaim_if_expired(key);
    misses_.fetch_add(1, std::memory_order_relaxed);
    return std::nullopt;
  }

  // Inserts or overwrites. An overwrite replaces the expiry as well: no ttl
  // means the new entry never expires, regardless of the old entry's TTL.
  void set(const CacheKey& key, std::string response,
           std::optional<Clock::Duration> ttl = std::nullopt) {
    std::optional<Clock::TimePoint> expires_at;
    if (ttl.has_value()) {
      expires_at = clock_.now() + *ttl;
    }
    std::unique_lock<std::shared_mutex> lock(mutex_);
    const auto [it, inserted] = entries_.try_emplace(key);
    if (inserted) {
      size_bytes_ += key.bytes().size();
    } else {
      size_bytes_ -= it->second.response.size();
    }
    size_bytes_ += response.size();
    it->second = Entry{std::move(response), expires_at};
  }

  // Returns true iff an entry was removed (expired-but-unreclaimed included).
  bool erase(const CacheKey& key) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    const auto it = entries_.find(key);
    if (it == entries_.end()) {
      return false;
    }
    erase_locked(it);
    return true;
  }

  // Reclaims every expired entry. Returns the number removed.
  std::size_t sweep() {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    const Clock::TimePoint now = clock_.now();
    std::size_t removed = 0;
    for (auto it = entries_.begin(); it != entries_.end();) {
      if (it->second.expires_at.has_value() && now >= *it->second.expires_at) {
        size_bytes_ -= it->first.bytes().size() + it->second.response.size();
        it = entries_.erase(it);
        ++removed;
      } else {
        ++it;
      }
    }
    expirations_ += removed;
    return removed;
  }

  [[nodiscard]] Stats stats() const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    Stats stats;
    stats.hits = hits_.load(std::memory_order_relaxed);
    stats.misses = misses_.load(std::memory_order_relaxed);
    stats.expirations = expirations_;
    stats.entries = entries_.size();
    stats.size_bytes = size_bytes_;
    return stats;
  }

 private:
  struct Entry {
    std::string response;
    std::optional<Clock::TimePoint> expires_at;
  };

  [[nodiscard]] bool expired(const Entry& entry) const {
    return entry.expires_at.has_value() && clock_.now() >= *entry.expires_at;
  }

  // Re-checks under the exclusive lock: between the shared-lock observation
  // and this call, a set() may have replaced the entry with a fresh one,
  // which must not be reclaimed.
  void reclaim_if_expired(const CacheKey& key) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    const auto it = entries_.find(key);
    if (it != entries_.end() && expired(it->second)) {
      erase_locked(it);
      ++expirations_;
    }
  }

  using EntryMap = std::unordered_map<CacheKey, Entry, CacheKey::Hash>;

  void erase_locked(EntryMap::iterator it) {
    size_bytes_ -= it->first.bytes().size() + it->second.response.size();
    entries_.erase(it);
  }

  const Clock& clock_;
  mutable std::shared_mutex mutex_;
  EntryMap entries_;
  std::size_t size_bytes_ = 0;  // mutated under exclusive lock only
  std::size_t expirations_ = 0; // mutated under exclusive lock only
  mutable std::atomic<std::size_t> hits_{0};
  mutable std::atomic<std::size_t> misses_{0};
};

} // namespace replayarena
