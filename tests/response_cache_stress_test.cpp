#include "gateway/cache_key.h"
#include "gateway/clock.h"
#include "gateway/response_cache.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <gtest/gtest.h>
#include <random>
#include <string>
#include <thread>
#include <vector>

// Concurrency test for the complete ResponseCache (stages 1+2+3). Must stay
// silent under TSan in CI. Fixed seeds: a failure here must reproduce. Time
// is driven by a SimulatedClock advanced from a dedicated thread, so TTL
// expiry races get, set, erase, and sweep; the byte cap is sized well below
// the working set, so LRU eviction races everything too.
namespace replayarena {
namespace {

using namespace std::chrono_literals;

TEST(ResponseCacheStress, MixedOpsWithTtlEvictionAndConcurrentClockAdvance) {
  constexpr std::size_t kThreads = 8;
  constexpr std::size_t kOpsPerThread = 5000;
  constexpr std::size_t kKeySpace = 64;
  static constexpr std::size_t kByteCap = 2000; // well below the ~4.5KB working set

  SimulatedClock clock;
  ResponseCache cache(clock, kByteCap);
  std::vector<CacheKey> keys;
  keys.reserve(kKeySpace);
  for (std::size_t i = 0; i < kKeySpace; ++i) {
    keys.push_back(CacheKey::make("model", "prompt-" + std::to_string(i), {{"seed", "7"}}));
  }

  std::atomic<bool> stop_advancing{false};
  std::thread advancer([&clock, &stop_advancing] {
    while (!stop_advancing.load(std::memory_order_acquire)) {
      clock.advance(1ms);
      std::this_thread::yield();
    }
  });

  std::atomic<std::size_t> local_gets{0};
  std::vector<std::thread> workers;
  workers.reserve(kThreads);
  for (std::size_t t = 0; t < kThreads; ++t) {
    workers.emplace_back([&cache, &keys, &local_gets, t] {
      std::mt19937 rng(static_cast<std::mt19937::result_type>(t) + 1);
      for (std::size_t i = 0; i < kOpsPerThread; ++i) {
        const CacheKey& k = keys[rng() % kKeySpace];
        switch (rng() % 8) {
        case 0:
          cache.set(k, "payload-" + std::to_string(rng() % 16));
          break;
        case 1:
        case 2:
          // Short TTLs, same order of magnitude as the advancer's steps, so
          // expiry constantly races the other operations.
          cache.set(k, "payload-" + std::to_string(rng() % 16),
                    std::chrono::milliseconds(1 + rng() % 5));
          break;
        case 3:
          (void)cache.erase(k);
          break;
        case 4:
          (void)cache.sweep();
          break;
        default: {
          const auto got = cache.get(k);
          if (got.has_value()) {
            // A hit must always be a well-formed payload, never torn bytes.
            EXPECT_EQ(got->rfind("payload-", 0), 0u);
          }
          ++local_gets;
          if (i % 256 == 0) {
            // The cap invariant must hold at every observable instant.
            EXPECT_LE(cache.stats().size_bytes, kByteCap);
          }
          break;
        }
        }
      }
    });
  }
  for (auto& w : workers) {
    w.join();
  }
  stop_advancing.store(true, std::memory_order_release);
  advancer.join();

  const auto stats = cache.stats();
  EXPECT_EQ(stats.hits + stats.misses, local_gets.load());
  EXPECT_LE(stats.entries, kKeySpace);
  EXPECT_LE(stats.size_bytes, kByteCap);

  // Draining everything must zero the byte accounting exactly.
  for (const auto& k : keys) {
    (void)cache.erase(k);
  }
  EXPECT_EQ(cache.stats().entries, 0u);
  EXPECT_EQ(cache.stats().size_bytes, 0u);
}

} // namespace
} // namespace replayarena
