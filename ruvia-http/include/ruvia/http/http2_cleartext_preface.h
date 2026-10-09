#pragma once

#include <cstdint>
#include <string_view>

#include "ruvia/http/http2_framing.h"

namespace ruvia {

enum class http2_cleartext_preface_probe : std::uint8_t {
    http1,
    need_more_preface,
    complete_preface,
    drop_connection,
};

// Classify bytes that arrived on a cleartext listener before HTTP/1 parsing.
// A matching or truncated client preface is prior-knowledge HTTP/2. A `PRI `
// prefix that cannot become the preface is dropped. Anything else is HTTP/1.
[[nodiscard]] inline http2_cleartext_preface_probe probe_http2_cleartext_preface(
    std::string_view current) noexcept {
    if (current.empty()) {
        return http2_cleartext_preface_probe::http1;
    }

    if (current.starts_with(::ruvia::http2_client_preface) ||
        ::ruvia::http2_client_preface.starts_with(current)) {
        return current.size() >= ::ruvia::http2_client_preface.size()
                   ? http2_cleartext_preface_probe::complete_preface
                   : http2_cleartext_preface_probe::need_more_preface;
    }

    if (current.starts_with("PRI ")) {
        return http2_cleartext_preface_probe::drop_connection;
    }
    return http2_cleartext_preface_probe::http1;
}

}  // namespace ruvia
