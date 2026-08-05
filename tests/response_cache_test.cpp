#include "gateway/cache_key.h"
#include "gateway/response_cache.h"

#include <gtest/gtest.h>
#include <string>

namespace replayarena {
namespace {

CacheKey key(const std::string& name) {
  return CacheKey::make("model", name, {});
}

TEST(ResponseCache, MissThenHit) {
  ResponseCache cache;
  EXPECT_EQ(cache.get(key("a")), std::nullopt);
  cache.set(key("a"), "response-a");
  EXPECT_EQ(cache.get(key("a")), std::optional<std::string>("response-a"));
}

TEST(ResponseCache, HitReturnsBytesIdentically) {
  ResponseCache cache;
  const std::string payload{"bin\0ary\xFF", 8};
  cache.set(key("a"), payload);
  const auto got = cache.get(key("a"));
  ASSERT_TRUE(got.has_value());
  EXPECT_EQ(*got, payload);
  EXPECT_EQ(got->size(), 8u);
}

TEST(ResponseCache, SetOverwrites) {
  ResponseCache cache;
  cache.set(key("a"), "old");
  cache.set(key("a"), "new");
  EXPECT_EQ(cache.get(key("a")), std::optional<std::string>("new"));
  EXPECT_EQ(cache.stats().entries, 1u);
}

TEST(ResponseCache, EraseRemovesAndReportsPresence) {
  ResponseCache cache;
  cache.set(key("a"), "x");
  EXPECT_TRUE(cache.erase(key("a")));
  EXPECT_FALSE(cache.erase(key("a")));
  EXPECT_EQ(cache.get(key("a")), std::nullopt);
}

TEST(ResponseCache, StatsCountHitsAndMisses) {
  ResponseCache cache;
  cache.set(key("a"), "x");
  (void)cache.get(key("a")); // hit
  (void)cache.get(key("b")); // miss
  (void)cache.get(key("b")); // miss
  const auto stats = cache.stats();
  EXPECT_EQ(stats.hits, 1u);
  EXPECT_EQ(stats.misses, 2u);
  EXPECT_EQ(stats.evictions, 0u);
}

TEST(ResponseCache, SizeBytesTracksKeyAndPayload) {
  ResponseCache cache;
  const auto k = key("a");
  cache.set(k, "12345");
  EXPECT_EQ(cache.stats().size_bytes, k.bytes().size() + 5u);

  cache.set(k, "123"); // overwrite shrinks payload accounting
  EXPECT_EQ(cache.stats().size_bytes, k.bytes().size() + 3u);

  EXPECT_TRUE(cache.erase(k));
  EXPECT_EQ(cache.stats().size_bytes, 0u);
  EXPECT_EQ(cache.stats().entries, 0u);
}

TEST(ResponseCache, IdenticalOperationSequencesProduceIdenticalCaches) {
  // Determinism sanity for the replay story: same ops in, same state out.
  ResponseCache a;
  ResponseCache b;
  for (ResponseCache* cache : {&a, &b}) {
    cache->set(key("x"), "1");
    cache->set(key("y"), "2");
    cache->set(key("x"), "3");
    cache->erase(key("y"));
    (void)cache->get(key("x"));
    (void)cache->get(key("z"));
  }
  EXPECT_EQ(a.get(key("x")), b.get(key("x")));
  EXPECT_EQ(a.stats().entries, b.stats().entries);
  EXPECT_EQ(a.stats().size_bytes, b.stats().size_bytes);
}

} // namespace
} // namespace replayarena
