#pragma once

#include "gateway/backend.h"
#include "gateway/clock.h"
#include "gateway/request.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace replayarena {

// Deterministic backend for tests and benchmarks (issue #3).
//
// Output is a pure function of (backend seed, request model, request
// prompt): same inputs, same bytes, on any platform, forever. That makes
// scheduler tests assertable and end-to-end determinism tests meaningful.
//
// simulated_latency models per-batch backend work with a real sleep on the
// calling worker thread. This is not a wall-clock read and never shapes
// output bytes or any decision; it only makes workers occupy real time so
// batching behavior is observable in benchmarks. Test configurations use
// zero latency. Stateless, hence trivially thread-safe.
class MockBackend final : public Backend {
 public:
  explicit MockBackend(std::uint64_t seed, Clock::Duration simulated_latency = {})
      : seed_(seed), simulated_latency_(simulated_latency) {}

  [[nodiscard]] std::vector<BackendResponse> execute(const Batch& batch) override {
    if (simulated_latency_ > Clock::Duration::zero()) {
      std::this_thread::sleep_for(simulated_latency_);
    }
    std::vector<BackendResponse> responses;
    responses.reserve(batch.requests.size());
    for (const Request& request : batch.requests) {
      responses.push_back(BackendResponse{request.id, respond_to(request)});
    }
    return responses;
  }

 private:
  // FNV-1a 64-bit over seed and request content. Not cryptographic and not
  // meant to be: it only has to be deterministic and cheap.
  [[nodiscard]] std::string respond_to(const Request& request) const {
    std::uint64_t hash = 14695981039346656037ULL;
    const auto mix = [&hash](std::string_view bytes) {
      for (const char c : bytes) {
        hash ^= static_cast<std::uint8_t>(c);
        hash *= 1099511628211ULL;
      }
      hash ^= 0xFF; // field separator, so ("ab","c") differs from ("a","bc")
      hash *= 1099511628211ULL;
    };
    // Seed bytes mixed in fixed (little-endian) order so output does not
    // depend on host endianness.
    for (int shift = 0; shift < 64; shift += 8) {
      hash ^= (seed_ >> shift) & 0xFF;
      hash *= 1099511628211ULL;
    }
    mix(request.model);
    mix(request.prompt);

    std::string output = "mock-";
    constexpr char kHex[] = "0123456789abcdef";
    for (int shift = 60; shift >= 0; shift -= 4) {
      output.push_back(kHex[static_cast<std::size_t>((hash >> shift) & 0xF)]);
    }
    return output;
  }

  const std::uint64_t seed_;
  const Clock::Duration simulated_latency_;
};

} // namespace replayarena
