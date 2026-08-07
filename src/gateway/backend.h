#pragma once

#include "gateway/request.h"

#include <vector>

namespace replayarena {

// A model backend (issue #3). Implementations: MockBackend (deterministic,
// for tests and benchmarks), Ollama and OpenAI-compatible HTTP adapters
// (v0.3), and the replay backend serving recorded responses (issue #4).
//
// Contract:
// - execute() is synchronous and is called from worker-pool threads, so
//   implementations must be thread-safe.
// - Exactly one response per request, in request order, matched by id.
//   Backend-level failures are reported inside the response payload by the
//   adapter (no exceptions cross this boundary, SPEC.md section 7).
// - Implementations must not read clocks to shape output content: response
//   bytes must be a pure function of the request and backend configuration,
//   or (for real model backends) captured verbatim into traces so replay
//   never depends on backend determinism (SPEC.md section 6).
class Backend {
 public:
  Backend() = default;
  Backend(const Backend&) = delete;
  Backend& operator=(const Backend&) = delete;
  virtual ~Backend() = default;

  [[nodiscard]] virtual std::vector<BackendResponse> execute(const Batch& batch) = 0;
};

} // namespace replayarena
