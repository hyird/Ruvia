#pragma once

#include <cstdint>
#include <string_view>

#include "ruvia/http/Http2Framing.h"

namespace ruvia {

enum class Http2CleartextPrefaceProbe : std::uint8_t {
    kHttp1,
    kNeedMorePreface,
    kCompletePreface,
    kDropConnection,
};

// Classify bytes that arrived on a cleartext listener before HTTP/1 parsing.
// A matching or truncated client preface is prior-knowledge HTTP/2. A `PRI `
// prefix that cannot become the preface is dropped. Anything else is HTTP/1.
[[nodiscard]] inline Http2CleartextPrefaceProbe probeHttp2CleartextPreface(
    std::string_view current) noexcept {
    if (current.empty()) {
        return Http2CleartextPrefaceProbe::kHttp1;
    }

    if (current.starts_with(::ruvia::kHttp2ClientPreface) ||
        ::ruvia::kHttp2ClientPreface.starts_with(current)) {
        return current.size() >= ::ruvia::kHttp2ClientPreface.size()
                   ? Http2CleartextPrefaceProbe::kCompletePreface
                   : Http2CleartextPrefaceProbe::kNeedMorePreface;
    }

    if (current.starts_with("PRI ")) {
        return Http2CleartextPrefaceProbe::kDropConnection;
    }
    return Http2CleartextPrefaceProbe::kHttp1;
}

}  // namespace ruvia
