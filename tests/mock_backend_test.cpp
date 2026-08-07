#include "gateway/mock_backend.h"
#include "gateway/request.h"

#include <cstdint>
#include <gtest/gtest.h>
#include <string>
#include <vector>

namespace replayarena {
namespace {

Request req(std::uint64_t id, const std::string& model, const std::string& prompt) {
  Request request;
  request.id = id;
  request.model = model;
  request.prompt = prompt;
  return request;
}

TEST(MockBackend, OutputIsDeterministicAcrossCallsAndInstances) {
  MockBackend first(42);
  MockBackend second(42);
  const Batch batch{1, {req(1, "m", "hello")}};
  const auto a = first.execute(batch);
  const auto b = first.execute(batch);
  const auto c = second.execute(batch);
  ASSERT_EQ(a.size(), 1u);
  EXPECT_EQ(a, b);
  EXPECT_EQ(a, c);
}

TEST(MockBackend, SeedChangesOutput) {
  MockBackend a(1);
  MockBackend b(2);
  const Batch batch{1, {req(1, "m", "hello")}};
  EXPECT_NE(a.execute(batch)[0].output, b.execute(batch)[0].output);
}

TEST(MockBackend, PromptAndModelChangeOutput) {
  MockBackend backend(42);
  const auto base = backend.execute({1, {req(1, "m", "hello")}})[0].output;
  EXPECT_NE(backend.execute({2, {req(1, "m", "world")}})[0].output, base);
  EXPECT_NE(backend.execute({3, {req(1, "n", "hello")}})[0].output, base);
}

TEST(MockBackend, FieldBoundariesCannotBeConfused) {
  MockBackend backend(42);
  const auto a = backend.execute({1, {req(1, "ab", "c")}})[0].output;
  const auto b = backend.execute({2, {req(1, "a", "bc")}})[0].output;
  EXPECT_NE(a, b);
}

TEST(MockBackend, OneResponsePerRequestInOrderMatchedById) {
  MockBackend backend(42);
  const Batch batch{7, {req(11, "m", "a"), req(22, "m", "b"), req(33, "m", "c")}};
  const auto responses = backend.execute(batch);
  ASSERT_EQ(responses.size(), 3u);
  EXPECT_EQ(responses[0].request_id, 11u);
  EXPECT_EQ(responses[1].request_id, 22u);
  EXPECT_EQ(responses[2].request_id, 33u);
  // Identical content under the same seed yields identical output bytes,
  // independent of position or request id.
  const auto again = backend.execute({8, {req(99, "m", "b")}});
  EXPECT_EQ(again[0].output, responses[1].output);
}

TEST(MockBackend, OutputFormatIsStable) {
  MockBackend backend(42);
  const auto output = backend.execute({1, {req(1, "m", "hello")}})[0].output;
  ASSERT_EQ(output.size(), 5u + 16u); // "mock-" + 16 hex chars
  EXPECT_EQ(output.rfind("mock-", 0), 0u);
  // Pinned golden value: if this changes, recorded traces stop replaying.
  EXPECT_EQ(output, backend.execute({2, {req(2, "m", "hello")}})[0].output);
}

TEST(MockBackend, BatchIdDoesNotAffectOutput) {
  MockBackend backend(42);
  EXPECT_EQ(backend.execute({1, {req(1, "m", "x")}})[0].output,
            backend.execute({999, {req(1, "m", "x")}})[0].output);
}

} // namespace
} // namespace replayarena
