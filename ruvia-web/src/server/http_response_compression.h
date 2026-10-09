#pragma once

#include <cstdint>

#include "ruvia/core/blocking_pool.h"
#include "ruvia/core/task.h"
#include "ruvia/core/worker_handle.h"
#include "ruvia/http/http_accept_encoding.h"
#include "ruvia/http/http_field_values.h"
#include "ruvia/http/http_known_method.h"
#include "ruvia/http/http_response.h"
#include "ruvia/http/http_response_server.h"
#include "ruvia/web/server_config.h"

namespace ruvia::detail {

[[nodiscard]] inline bool response_has_header_name(
    const http_response& response, std::string_view name) noexcept {
    for (const auto& header : response.headers()) {
        if (http_ascii_equals_ignore_case(header.name(), name)) {
            return true;
        }
    }
    return false;
}

// Runtime capability is deliberately separate from Accept-Encoding
// negotiation. A precompressed file sidecar can satisfy a selected coding
// without this process owning an incremental encoder.
enum class http_response_coding_availability : std::uint8_t {
    identity_only,
    identity_and_compression,
};

// The representation source affects compression eligibility (notably SSE's
// media type), while static-file representations remain owned by the static
// response path.
enum class http_response_compression_source : std::uint8_t {
    buffered,
    stream,
    sse,
};

enum class http_response_compression_decision : std::uint8_t {
    fixed_representation,
    negotiated_identity,
    encode,
};

// Compression policy and encoder execution are separate outcomes. A response
// may intentionally remain identity (body too small, no-transform, an
// incompressible media type, or an unsupported runtime capability), or the
// encoder may actually fail. Those cases have different meanings when the
// client has forbidden identity and must not collapse back to a bool.
enum class http_response_compression_status : std::uint8_t {
    compressed,
    not_applicable,
    failed,
};

class http_response_compression_result final {
public:
    [[nodiscard]] static constexpr http_response_compression_result make_compressed() noexcept {
        return http_response_compression_result(http_response_compression_status::compressed);
    }

    [[nodiscard]] static constexpr http_response_compression_result make_not_applicable() noexcept {
        return http_response_compression_result(http_response_compression_status::not_applicable);
    }

    [[nodiscard]] static constexpr http_response_compression_result make_failed() noexcept {
        return http_response_compression_result(http_response_compression_status::failed);
    }

    [[nodiscard]] constexpr http_response_compression_status status() const noexcept {
        return status_;
    }

    [[nodiscard]] constexpr bool compressed() const noexcept {
        return status_ == http_response_compression_status::compressed;
    }

    [[nodiscard]] constexpr bool not_applicable() const noexcept {
        return status_ == http_response_compression_status::not_applicable;
    }

    [[nodiscard]] constexpr bool failed() const noexcept {
        return status_ == http_response_compression_status::failed;
    }

private:
    constexpr explicit http_response_compression_result(http_response_compression_status status) noexcept
        : status_(status) {}

    http_response_compression_status status_;
};

// Response negotiation inspects every known coding in application-supplied
// stacks: a client that accepts gzip but rejects br cannot be sent `gzip, br`.
[[nodiscard]] inline bool http_known_response_content_encoding_stack_accepted(
    const http_response_coding_selection& selection, const http_response& response) noexcept {
    bool saw_known_coding = false;
    bool saw_unknown_coding = false;
    bool accepted = true;
    for (const auto& header : response.headers()) {
        if (!http_ascii_equals_ignore_case(header.name(), "Content-Encoding")) {
            continue;
        }
        http_visit_comma_separated_quoted_field_items(header.value(), [&](std::string_view item) noexcept {
            if (item.empty()) {
                saw_unknown_coding = true;
            } else if (http_ascii_equals_ignore_case(item, "gzip") ||
                       http_ascii_equals_ignore_case(item, "x-gzip")) {
                saw_known_coding = true;
                accepted = accepted && selection.accepts(http_content_coding::gzip);
            } else if (http_ascii_equals_ignore_case(item, "deflate")) {
                saw_known_coding = true;
                accepted = accepted && selection.accepts(http_content_coding::deflate);
            } else if (http_ascii_equals_ignore_case(item, "br")) {
                saw_known_coding = true;
                accepted = accepted && selection.accepts(http_content_coding::brotli);
            } else if (http_ascii_equals_ignore_case(item, "zstd")) {
                saw_known_coding = true;
                accepted = accepted && selection.accepts(http_content_coding::zstd);
            } else if (http_ascii_equals_ignore_case(item, "identity")) {
                saw_known_coding = true;
                accepted = accepted && selection.accepts(http_content_coding::identity);
            } else {
                // A custom coding (or malformed sender-side list member) is
                // outside the framework's decoder/registry. Leave that full
                // stack under application ownership instead of guessing.
                saw_unknown_coding = true;
            }
            return true;
        });
    }
    return !saw_known_coding || saw_unknown_coding || accepted;
}

// Return true when the response cannot satisfy the client's coding policy:
// either a framework-selected coding fell back to forbidden identity, or a
// handler supplied a known pre-encoded representation the client excluded.
// Bodyless statuses are representation-free and therefore never need a
// content-coding fallback.
[[nodiscard]] inline bool http_response_coding_fallback_forbidden(
    const http_response_coding_selection& selection, http_known_method request_method,
    const http_response& response) noexcept {
    if (!plan_http_response_body(request_method, response.status()).status_allows_body()) {
        return false;
    }
    if (response_has_header_name(response, "Content-Encoding")) {
        // Custom and malformed application-managed coding stacks remain the
        // application's responsibility; every fully known stack is negotiated
        // member by member without allocating in this noexcept decision.
        return !http_known_response_content_encoding_stack_accepted(selection, response);
    }
    if (selection.coding() == http_content_coding::identity || selection.identity_accepted()) {
        return false;
    }
    // This guard targets only the framework's silent identity fallback.
    return true;
}

[[nodiscard]] http_response_compression_result apply_response_compression(
    const http_response_coding_selection& selection, http_known_method request_method,
    http_response& response, const compression_config& options);

// Applies the same policy as apply_response_compression(), but offloads an
// in-memory body above sync_bytes_ to the bounded blocking pool. A disabled pool
// extends synchronous compression through max_bytes; rejection by an existing
// pool is an intentional identity fallback. Encoder/commit failures remain
// typed failures so a client that forbids identity receives a terminal error.
[[nodiscard]] task<http_response_compression_result> apply_response_compression_async(
    const http_response_coding_selection& selection, http_known_method request_method,
    http_response& response, compression_config options, blocking_pool* pool,
    const worker_handle& worker_value);

// Shared Web response policy. Fixed representations do not vary by coding;
// eligible representations are annotated before deciding between identity
// and an encoder. Encoder availability never changes Accept-Encoding parsing.
[[nodiscard]] http_response_compression_decision prepare_response_compression(
    const http_response_coding_selection& selection, http_known_method request_method,
    http_response& response, http_response_compression_source source_value,
    http_response_coding_availability availability);

}  // namespace ruvia::detail
