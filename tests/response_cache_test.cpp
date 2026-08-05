#include "gateway/cache_key.h"
#include "gateway/clock.h"
#include "gateway/response_cache.h"

#include <chrono>
#include <gtest/gtest.h>
#include <string>

namespace replayarena {
namespace {

using namespace std::chrono_literals;

CacheKey key(const std::string& name) {
  return CacheKey::make("model", name, {});
}

// All cache tests run against SimulatedClock: TTL behavior is stepped to
// exact instants, and no test ever sleeps.
class ResponseCacheTest : public ::testing::Test {
 protected:
  SimulatedClock clock_;
  ResponseCache cache_{clock_};
};

TEST_F(ResponseCacheTest, MissThenHit) {
  EXPECT_EQ(cache_.get(key("a")), std::nullopt);
  cache_.set(key("a"), "response-a");
  EXPECT_EQ(cache_.get(key("a")), std::optional<std::string>("response-a"));
}

TEST_F(ResponseCacheTest, HitReturnsBytesIdentically) {
  const std::string payload{"bin\0ary\xFF", 8};
  cache_.set(key("a"), payload);
  const auto got = cache_.get(key("a"));
  ASSERT_TRUE(got.has_value());
  EXPECT_EQ(*got, payload);
  EXPECT_EQ(got->size(), 8u);
}

TEST_F(ResponseCacheTest, SetOverwrites) {
  cache_.set(key("a"), "old");
  cache_.set(key("a"), "new");
  EXPECT_EQ(cache_.get(key("a")), std::optional<std::string>("new"));
  EXPECT_EQ(cache_.stats().entries, 1u);
}

TEST_F(ResponseCacheTest, EraseRemovesAndReportsPresence) {
  cache_.set(key("a"), "x");
  EXPECT_TRUE(cache_.erase(key("a")));
  EXPECT_FALSE(cache_.erase(key("a")));
  EXPECT_EQ(cache_.get(key("a")), std::nullopt);
}

TEST_F(ResponseCacheTest, StatsCountHitsAndMisses) {
  cache_.set(key("a"), "x");
  (void)cache_.get(key("a")); // hit
  (void)cache_.get(key("b")); // miss
  (void)cache_.get(key("b")); // miss
  const auto stats = cache_.stats();
  EXPECT_EQ(stats.hits, 1u);
  EXPECT_EQ(stats.misses, 2u);
  EXPECT_EQ(stats.evictions, 0u);
}

TEST_F(ResponseCacheTest, SizeBytesTracksKeyAndPayload) {
  const auto k = key("a");
  cache_.set(k, "12345");
  EXPECT_EQ(cache_.stats().size_bytes, k.bytes().size() + 5u);

  cache_.set(k, "123"); // overwrite shrinks payload accounting
  EXPECT_EQ(cache_.stats().size_bytes, k.bytes().size() + 3u);

  EXPECT_TRUE(cache_.erase(k));
  EXPECT_EQ(cache_.stats().size_bytes, 0u);
  EXPECT_EQ(cache_.stats().entries, 0u);
}

// ---- Stage 2: TTL ----

TEST_F(ResponseCacheTest, EntryWithTtlIsVisibleStrictlyBeforeExpiry) {
  cache_.set(key("a"), "x", 10ms);
  clock_.advance(10ms - Clock::Duration{1});
  EXPECT_EQ(cache_.get(key("a")), std::optional<std::string>("x"));
}

TEST_F(ResponseCacheTest, EntryExpiresExactlyAtBoundary) {
  cache_.set(key("a"), "x", 10ms);
  clock_.advance(10ms);
  EXPECT_EQ(cache_.get(key("a")), std::nullopt);
}

TEST_F(ResponseCacheTest, ExpiredGetCountsAsMissAndReclaims) {
  const auto k = key("a");
  cache_.set(k, "x", 5ms);
  clock_.advance(6ms);
  EXPECT_EQ(cache_.get(k), std::nullopt);
  const auto stats = cache_.stats();
  EXPECT_EQ(stats.misses, 1u);
  EXPECT_EQ(stats.expirations, 1u);
  EXPECT_EQ(stats.entries, 0u);
  EXPECT_EQ(stats.size_bytes, 0u);
}

TEST_F(ResponseCacheTest, EntryWithoutTtlNeverExpires) {
  cache_.set(key("a"), "x");
  clock_.advance(std::chrono::hours(24 * 365));
  EXPECT_EQ(cache_.get(key("a")), std::optional<std::string>("x"));
  EXPECT_EQ(cache_.sweep(), 0u);
}

TEST_F(ResponseCacheTest, OverwriteReplacesTtl) {
  cache_.set(key("a"), "short-lived", 5ms);
  clock_.advance(3ms);
  cache_.set(key("a"), "long-lived", 100ms);
  clock_.advance(90ms); // far past the original expiry
  EXPECT_EQ(cache_.get(key("a")), std::optional<std::string>("long-lived"));
}

TEST_F(ResponseCacheTest, OverwriteWithoutTtlClearsExpiry) {
  cache_.set(key("a"), "mortal", 5ms);
  cache_.set(key("a"), "immortal");
  clock_.advance(std::chrono::hours(1));
  EXPECT_EQ(cache_.get(key("a")), std::optional<std::string>("immortal"));
}

TEST_F(ResponseCacheTest, SweepRemovesOnlyExpiredEntries) {
  const auto dead_a = key("dead-a");
  const auto dead_b = key("dead-b");
  cache_.set(dead_a, "1", 5ms);
  cache_.set(dead_b, "22", 7ms);
  cache_.set(key("alive"), "333", 100ms);
  cache_.set(key("forever"), "4444");

  clock_.advance(10ms);
  EXPECT_EQ(cache_.sweep(), 2u);

  const auto stats = cache_.stats();
  EXPECT_EQ(stats.expirations, 2u);
  EXPECT_EQ(stats.entries, 2u);
  EXPECT_EQ(stats.size_bytes,
            key("alive").bytes().size() + 3u + key("forever").bytes().size() + 4u);
  EXPECT_EQ(cache_.get(dead_a), std::nullopt);
  EXPECT_EQ(cache_.get(key("alive")), std::optional<std::string>("333"));
}

TEST_F(ResponseCacheTest, SweepIsIdempotent) {
  cache_.set(key("a"), "x", 5ms);
  clock_.advance(10ms);
  EXPECT_EQ(cache_.sweep(), 1u);
  EXPECT_EQ(cache_.sweep(), 0u);
  EXPECT_EQ(cache_.stats().expirations, 1u);
}

TEST_F(ResponseCacheTest, ExpiredEntryStillCountsInEntriesUntilReclaimed) {
  cache_.set(key("a"), "x", 5ms);
  clock_.advance(10ms);
  // Not yet observed by get or sweep: still occupies an entry, but must be
  // invisible to readers.
  EXPECT_EQ(cache_.stats().entries, 1u);
  EXPECT_EQ(cache_.get(key("a")), std::nullopt);
  EXPECT_EQ(cache_.stats().entries, 0u);
}

TEST_F(ResponseCacheTest, IdenticalOperationSequencesProduceIdenticalCaches) {
  // Determinism sanity for the replay story: same ops and same clock steps
  // in, same state out.
  SimulatedClock clock_a;
  SimulatedClock clock_b;
  ResponseCache a{clock_a};
  ResponseCache b{clock_b};
  const auto run = [](ResponseCache& cache, SimulatedClock& clock) {
    cache.set(key("x"), "1", 5ms);
    cache.set(key("y"), "2");
    clock.advance(3ms);
    cache.set(key("x"), "3", 5ms); // refreshed before expiry
    clock.advance(4ms);            // original TTL passed, refreshed one has not
    (void)cache.get(key("x"));
    (void)cache.get(key("z"));
    cache.erase(key("y"));
    clock.advance(10ms);
    (void)cache.sweep();
  };
  run(a, clock_a);
  run(b, clock_b);
  EXPECT_EQ(a.get(key("x")), b.get(key("x")));
  EXPECT_EQ(a.stats().entries, b.stats().entries);
  EXPECT_EQ(a.stats().size_bytes, b.stats().size_bytes);
  EXPECT_EQ(a.stats().expirations, b.stats().expirations);
}

} // namespace
} // namespace replayarena
