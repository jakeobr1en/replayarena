#include "gateway/cache_key.h"

#include <gtest/gtest.h>
#include <string>

namespace replayarena {
namespace {

TEST(CacheKey, IdenticalInputsProduceEqualKeys) {
  const auto a = CacheKey::make("m", "p", {{"temperature", "0.7"}, {"seed", "1"}});
  const auto b = CacheKey::make("m", "p", {{"temperature", "0.7"}, {"seed", "1"}});
  EXPECT_EQ(a, b);
  EXPECT_EQ(a.bytes(), b.bytes());
}

TEST(CacheKey, ParamOrderDoesNotMatter) {
  const auto a = CacheKey::make("m", "p", {{"seed", "1"}, {"temperature", "0.7"}});
  const auto b = CacheKey::make("m", "p", {{"temperature", "0.7"}, {"seed", "1"}});
  EXPECT_EQ(a, b);
}

TEST(CacheKey, EveryFieldIsSignificant) {
  const auto base = CacheKey::make("m", "p", {{"seed", "1"}});
  EXPECT_NE(base, CacheKey::make("M", "p", {{"seed", "1"}}));
  EXPECT_NE(base, CacheKey::make("m", "P", {{"seed", "1"}}));
  EXPECT_NE(base, CacheKey::make("m", "p", {{"seed", "2"}}));
  EXPECT_NE(base, CacheKey::make("m", "p", {}));
}

TEST(CacheKey, FieldBoundariesCannotBeConfused) {
  // Without length prefixes these would concatenate to the same bytes.
  EXPECT_NE(CacheKey::make("ab", "c", {}), CacheKey::make("a", "bc", {}));
  EXPECT_NE(CacheKey::make("m", "p", {{"ab", "c"}}), CacheKey::make("m", "p", {{"a", "bc"}}));
}

TEST(CacheKey, EmbeddedNulBytesAreSafe) {
  const std::string with_nul{"a\0b", 3};
  const auto a = CacheKey::make("m", with_nul, {});
  const auto b = CacheKey::make("m", "a", {});
  EXPECT_NE(a, b);
}

TEST(CacheKey, EmptyFieldsStayDistinct) {
  EXPECT_NE(CacheKey::make("a", "", {}), CacheKey::make("", "a", {}));
}

} // namespace
} // namespace replayarena
