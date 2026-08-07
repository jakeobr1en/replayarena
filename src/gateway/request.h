#pragma once

#include "gateway/cancel_token.h"
#include "gateway/clock.h"

#include <cstdint>
#include <string>
#include <vector>

namespace replayarena {

// A unit of work submitted to the gateway (issue #3). The id is assigned by
// the submitter and must be unique for the lifetime of a gateway instance:
// traces, events, and responses all reference requests by this id.
struct Request {
  std::uint64_t id = 0;
  std::string client_id;
  std::string model;
  std::string prompt;
  Clock::TimePoint deadline{};
  CancelTokenPtr cancel_token; // may be null: not cancellable

  friend bool operator==(const Request&, const Request&) = default;
};

// One backend answer, matched to its request by id. Output bytes are exact:
// what a backend returns here is what lands in traces and caches.
struct BackendResponse {
  std::uint64_t request_id = 0;
  std::string output;

  friend bool operator==(const BackendResponse&, const BackendResponse&) = default;
};

// A group of requests dispatched together. Batch ids are assigned by the
// scheduler, strictly increasing per gateway instance.
struct Batch {
  std::uint64_t id = 0;
  std::vector<Request> requests;
};

} // namespace replayarena
