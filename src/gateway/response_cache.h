#pragma once

#include "gateway/cache_key.h"
#include "gateway/clock.h"

#include <cassert>
#include <cstddef>
#include <list>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>

namespace replayarena {

// Thread-safe response cache, issue #2 complete: get/set/erase (stage 1),
// per-entry TTL (stage 2), and LRU eviction under a byte cap (stage 3).
//
// Time: all expiry decisions are evaluated against the injected Clock, never
// std::chrono directly, so expiry is fully deterministic under test and
// replay (SPEC.md section 6). An entry set at time t with TTL d is expired
// once now() >= t + d: visible strictly before the boundary, gone at it.
// Expired entries are never returned; they are reclaimed inline when a get
// observes them and in bulk by sweep().
//
// Locking: one plain mutex. Stage 1 used a shared_mutex, but stage 3's LRU
// bookkeeping makes every hit a writer (it reorders the recency list), so
// shared locking would only ever help miss-heavy loads; the simpler mutex
// also lets an expired get reclaim inline instead of upgrading locks. No
// throughput requirement exists to justify more machinery; revisit with
// v0.2 metrics in hand.
//
// Eviction: with a byte cap configured, set() evicts least-recently-used
// entries until size_bytes fits the cap, so size never exceeds the cap after
// set returns. "Use" means a hit or a set; expired entries keep their list
// position until reclaimed and are evicted like any other entry. Eviction
// order is a pure function of the operation sequence (list order, never map
// order), so identical access sequences evict identically - a determinism
// requirement (SPEC.md section 6). An entry larger than the whole cap is
// admitted and then immediately evicted itself: set() keeps the invariant,
// the entry just never survives it; callers that care can notice via stats.
//
// A hit returns the stored response byte-identical (a copy of the exact
// bytes that were set; no re-serialization). Iteration order of the
// underlying map is never exposed (sweep returns only a count), so it cannot
// leak into any recorded decision.
class ResponseCache {
 public:
  struct Stats {
    std::size_t hits = 0;
    std::size_t misses = 0;
    std::size_t expirations = 0; // entries reclaimed after TTL expiry
    std::size_t evictions = 0;   // entries removed by the LRU byte cap
    std::size_t entries = 0;     // live entries, including not-yet-reclaimed expired ones
    std::size_t size_bytes = 0;  // canonical key bytes + response bytes
  };

  // No cap (nullopt) means unbounded. Precondition: a configured cap is > 0.
  explicit ResponseCache(const Clock& clock,
                         std::optional<std::size_t> max_size_bytes = std::nullopt)
      : clock_(clock), max_size_bytes_(max_size_bytes) {
    assert(!max_size_bytes_.has_value() || *max_size_bytes_ > 0);
  }

  // An expired entry counts as a miss and is reclaimed before returning.
  [[nodiscard]] std::optional<std::string> get(const CacheKey& key) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = entries_.find(key);
    if (it == entries_.end()) {
      ++misses_;
      return std::nullopt;
    }
    if (expired(it->second)) {
      erase_locked(it);
      ++expirations_;
      ++misses_;
      return std::nullopt;
    }
    ++hits_;
    touch_locked(it->second);
    return it->second.response;
  }

  // Inserts or overwrites. An overwrite replaces the expiry as well: no ttl
  // means the new entry never expires, regardless of the old entry's TTL.
  void set(const CacheKey& key, std::string response,
           std::optional<Clock::Duration> ttl = std::nullopt) {
    std::optional<Clock::TimePoint> expires_at;
    if (ttl.has_value()) {
      expires_at = clock_.now() + *ttl;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    const auto [it, inserted] = entries_.try_emplace(key);
    if (inserted) {
      size_bytes_ += key.bytes().size();
      lru_.push_front(&it->first);
      it->second.lru_it = lru_.begin();
    } else {
      size_bytes_ -= it->second.response.size();
      touch_locked(it->second);
    }
    size_bytes_ += response.size();
    it->second.response = std::move(response);
    it->second.expires_at = expires_at;
    evict_over_cap_locked();
  }

  // Returns true iff an entry was removed (expired-but-unreclaimed included).
  bool erase(const CacheKey& key) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = entries_.find(key);
    if (it == entries_.end()) {
      return false;
    }
    erase_locked(it);
    return true;
  }

  // Reclaims every expired entry. Returns the number removed.
  std::size_t sweep() {
    std::lock_guard<std::mutex> lock(mutex_);
    const Clock::TimePoint now = clock_.now();
    std::size_t removed = 0;
    for (auto it = entries_.begin(); it != entries_.end();) {
      if (it->second.expires_at.has_value() && now >= *it->second.expires_at) {
        const auto doomed = it++;
        erase_locked(doomed);
        ++removed;
      } else {
        ++it;
      }
    }
    expirations_ += removed;
    return removed;
  }

  [[nodiscard]] Stats stats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    Stats stats;
    stats.hits = hits_;
    stats.misses = misses_;
    stats.expirations = expirations_;
    stats.evictions = evictions_;
    stats.entries = entries_.size();
    stats.size_bytes = size_bytes_;
    return stats;
  }

 private:
  // LruList stores pointers into the map's keys; unordered_map is node
  // based, so those pointers stay valid until the entry itself is erased.
  using LruList = std::list<const CacheKey*>; // front = most recently used

  struct Entry {
    std::string response;
    std::optional<Clock::TimePoint> expires_at;
    LruList::iterator lru_it;
  };

  using EntryMap = std::unordered_map<CacheKey, Entry, CacheKey::Hash>;

  [[nodiscard]] bool expired(const Entry& entry) const {
    return entry.expires_at.has_value() && clock_.now() >= *entry.expires_at;
  }

  void touch_locked(Entry& entry) { lru_.splice(lru_.begin(), lru_, entry.lru_it); }

  void erase_locked(EntryMap::iterator it) {
    size_bytes_ -= it->first.bytes().size() + it->second.response.size();
    lru_.erase(it->second.lru_it);
    entries_.erase(it);
  }

  void evict_over_cap_locked() {
    if (!max_size_bytes_.has_value()) {
      return;
    }
    while (size_bytes_ > *max_size_bytes_) {
      assert(!lru_.empty());
      const auto victim = entries_.find(*lru_.back());
      assert(victim != entries_.end());
      erase_locked(victim);
      ++evictions_;
    }
  }

  const Clock& clock_;
  const std::optional<std::size_t> max_size_bytes_;
  mutable std::mutex mutex_;
  EntryMap entries_;
  LruList lru_;
  std::size_t size_bytes_ = 0;
  std::size_t expirations_ = 0;
  std::size_t evictions_ = 0;
  std::size_t hits_ = 0;
  std::size_t misses_ = 0;
};

} // namespace replayarena
