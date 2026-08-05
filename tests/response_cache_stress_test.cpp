#include "gateway/cache_key.h"
#include "gateway/response_cache.h"

#include <atomic>
#include <cstddef>
#include <gtest/gtest.h>
#include <random>
#include <string>
#include <thread>
#include <vector>

// Concurrency test for ResponseCache stage 1. Must stay silent under TSan in
// CI. Fixed seeds: a failure here must reproduce.
namespace replayarena {
namespace {

TEST(ResponseCacheStress, MixedGetSetEraseFromManyThreads) {
  constexpr std::size_t kThreads = 8;
  constexpr std::size_t kOpsPerThread = 5000;
  constexpr std::size_t kKeySpace = 64;

  ResponseCache cache;
  std::vector<CacheKey> keys;
  keys.reserve(kKeySpace);
  for (std::size_t i = 0; i < kKeySpace; ++i) {
    keys.push_back(CacheKey::make("model", "prompt-" + std::to_string(i), {{"seed", "7"}}));
  }

  std::atomic<std::size_t> local_gets{0};
  std::vector<std::thread> workers;
  workers.reserve(kThreads);
  for (std::size_t t = 0; t < kThreads; ++t) {
    workers.emplace_back([&cache, &keys, &local_gets, t] {
      std::mt19937 rng(static_cast<std::mt19937::result_type>(t) + 1);
      for (std::size_t i = 0; i < kOpsPerThread; ++i) {
        const CacheKey& k = keys[rng() % kKeySpace];
        switch (rng() % 4) {
        case 0:
          cache.set(k, "payload-" + std::to_string(rng() % 16));
          break;
        case 1:
          (void)cache.erase(k);
          break;
        default: {
          const auto got = cache.get(k);
          if (got.has_value()) {
            // A hit must always be a well-formed payload, never torn bytes.
            EXPECT_EQ(got->rfind("payload-", 0), 0u);
          }
          ++local_gets;
          break;
        }
        }
      }
    });
  }
  for (auto& w : workers) {
    w.join();
  }

  const auto stats = cache.stats();
  EXPECT_EQ(stats.hits + stats.misses, local_gets.load());
  EXPECT_LE(stats.entries, kKeySpace);

  // Erasing everything must drain the byte accounting to exactly zero.
  for (const auto& k : keys) {
    (void)cache.erase(k);
  }
  EXPECT_EQ(cache.stats().entries, 0u);
  EXPECT_EQ(cache.stats().size_bytes, 0u);
}

} // namespace
} // namespace replayarena
