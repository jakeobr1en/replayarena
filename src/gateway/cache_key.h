#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace replayarena {

// Canonical identity of a model request for caching (issue #2).
//
// Two call sites can never disagree about what "the same request" is because
// canonicalization lives here, not at the call sites: fields are
// length-prefixed (no delimiter ambiguity, embedded NULs are safe) and
// sampling params are sorted by name then value, so parameter order at the
// call site does not matter.
//
// The canonical byte string itself is the key. A content hash (SHA-256)
// deliberately does not exist yet: nothing needs it until the trace recorder
// (issue #4) introduces content hashes for divergence reports, and this
// project does not hand-roll crypto ahead of need. std::hash below is used
// only for in-memory bucket placement and is never recorded, so it cannot
// affect determinism (SPEC.md section 6).
class CacheKey {
 public:
  using Param = std::pair<std::string, std::string>;

  static CacheKey make(std::string_view model, std::string_view prompt, std::vector<Param> params) {
    std::sort(params.begin(), params.end());
    std::string bytes;
    bytes.reserve(model.size() + prompt.size() + 64);
    append_field(bytes, model);
    append_field(bytes, prompt);
    append_length(bytes, params.size());
    for (const Param& param : params) {
      append_field(bytes, param.first);
      append_field(bytes, param.second);
    }
    return CacheKey(std::move(bytes));
  }

  [[nodiscard]] const std::string& bytes() const noexcept { return bytes_; }

  friend bool operator==(const CacheKey&, const CacheKey&) = default;

  struct Hash {
    std::size_t operator()(const CacheKey& key) const noexcept {
      return std::hash<std::string>{}(key.bytes_);
    }
  };

 private:
  explicit CacheKey(std::string bytes) : bytes_(std::move(bytes)) {}

  static void append_length(std::string& out, std::uint64_t length) {
    for (int shift = 0; shift < 64; shift += 8) {
      out.push_back(static_cast<char>((length >> shift) & 0xFF));
    }
  }

  static void append_field(std::string& out, std::string_view field) {
    append_length(out, field.size());
    out.append(field);
  }

  std::string bytes_;
};

} // namespace replayarena
