#pragma once

#include <cstdint>

#include "ruvia/core/blocking_pool.h"
#include "ruvia/core/task.h"
#include "ruvia/core/worker_handle.h"
#include "ruvia/http/http_accept_encoding.h"
#include "ruvia/http/http_field_values.h"
#include "ruvia/http/http_known_method.h"
#include "ruvia/http/http_request.h"
#include "ruvia/http/http_response.h"
#include "ruvia/http/http_response_server.h"
#include "ruvia/web/error.h"
#include "ruvia/web/server_config.h"

#include "server/worker_codecs.h"

namespace ruvia::detail {

// Runtime capability is deliberately separate from Accept-Encoding
// negotiation. A precompressed file sidecar can satisfy a selected coding
// without this process owning an incremental encoder.
enum class http_response_coding_availability : std::uint8_t {
    identity_only,
    identity_and_compression,
};

// Static-file representations remain owned by the static response path.
enum class http_response_compression_source : std::uint8_t {
    buffered,
    stream,
};

enum class http_response_compression_decision : std::uint8_t {
    fixed_representation,
    negotiated_identity,
    encode,
};

// Compression policy and encoder execution are separate outcomes. A response
// may intentionally remain identity (body too small while identity is
// acceptable, no-transform, integrity digest fields, an incompressible media
// type, or an unsupported runtime capability), or the encoder may actually
// fail. Those cases have different meanings when the client has forbidden
// identity and must not collapse back to a bool.
enum class http_response_compression_status : std::uint8_t {
    compressed,
    not_applicable,
    failed,
};

// The 406 every coding-policy path answers when the client accepts none of the
// codings a response can be sent with. Options rather than http_error_info so
// a throw site can build an http_error from the same value.
[[nodiscard]] constexpr http_error_info_options http_not_acceptable_coding_error() noexcept {
    return {.status_ = http_status::not_acceptable,
        .code_ = "not_acceptable",
        .message_ = "no acceptable response content coding"};
}

[[nodiscard]] inline bool http_response_custom_coding_accepted(
    const http_request* request, std::string_view coding) noexcept {
    if (request == nullptr || !request->header("Accept-Encoding").has_value()) {
        return true;
    }
    http_accepted_encoding_quality quality;
    for (const auto& header : request->headers()) {
        if (http_ascii_equals_ignore_case(header.name(), "Accept-Encoding")) {
            quality.update(header.value(), coding);
        }
    }
    return quality.accepts(false);
}

// Response negotiation inspects every coding in application-supplied
// stacks: a client that accepts gzip but rejects br cannot be sent `gzip, br`.
[[nodiscard]] inline bool http_known_response_content_encoding_stack_accepted(
    const http_response_coding_selection& selection, const http_response& response,
    bool negotiation_failed = false, const http_request* request = nullptr) noexcept {
    bool saw_coding = false;
    bool accepted = true;
    for (const auto& header : response.headers()) {
        if (!http_ascii_equals_ignore_case(header.name(), "Content-Encoding")) {
            continue;
        }
        http_visit_comma_separated_quoted_field_items(header.value(), [&](std::string_view item) noexcept {
            if (item.empty()) {
                return true;
            }
            saw_coding = true;
            if (const auto coding = http_content_coding_from_token(item)) {
                accepted = accepted && !negotiation_failed && selection.accepts(*coding);
            } else {
                accepted = accepted && http_response_custom_coding_accepted(request, item);
            }
            return accepted;
        });
        if (!accepted) {
            break;
        }
    }
    return !saw_coding || accepted;
}

// Return true when the response cannot satisfy the client's coding policy:
// either a framework-selected coding fell back to forbidden identity, or a
// handler supplied a pre-encoded representation the client excluded.
// Bodyless statuses are representation-free and therefore never need a
// content-coding fallback.
[[nodiscard]] inline bool http_response_coding_fallback_forbidden(
    const http_response_coding_selection& selection, http_known_method request_method,
    const http_response& response, bool negotiation_failed = false,
    const http_request* request = nullptr) noexcept {
    if (!plan_http_response_body(request_method, response.status()).status_allows_body()) {
        return false;
    }
    // An empty 416 carries no content and so no coding to negotiate; its
    // Content-Range describes the selected representation (RFC 9110 15.5.17).
    if (response.status() == http_status::range_not_satisfiable && response.body_bytes().empty()) {
        return false;
    }
    if (response.header("Content-Encoding").has_value()) {
        return !http_known_response_content_encoding_stack_accepted(
            selection, response, negotiation_failed, request);
    }
    if (negotiation_failed) {
        return true;
    }
    if (selection.coding() == http_content_coding::identity || selection.identity_accepted()) {
        return false;
    }
    // This guard targets only the framework's silent identity fallback.
    return true;
}

// Outcome of the synchronous compression phase for an in-memory body. An
// eligible body above sync_bytes_ with a blocking pool configured is left as
// identity and flagged for offload; every other outcome is final here.
struct http_buffered_compression_step final {
    http_response_compression_status status_;
    bool offload_;
};

// Applies the buffered compression policy without suspending: eligibility,
// size bounds, and encoding up to sync_bytes_ (through max_bytes_ when
// `offload_available` is false, i.e. no blocking pool is configured). Only an
// offload result requires compress_response_offloaded(), so identity and
// ineligible responses never create a coroutine frame. The inline encoder is
// the worker's reusable one: its state stays in worker memory, and only the
// encoded body comes from the response's resource.
[[nodiscard]] http_buffered_compression_step apply_response_compression(
    const http_response_coding_selection& selection, http_known_method request_method,
    http_response& response, const compression_config& options, worker_codecs& codecs, bool offload_available);

// Encodes the body flagged by apply_response_compression() on the bounded
// blocking pool with a codec of its own, since worker codecs never leave the
// worker. Rejection by the pool is an intentional identity fallback;
// encoder/commit failures remain typed failures so a client that forbids
// identity receives a terminal error.
[[nodiscard]] task<http_response_compression_status> compress_response_offloaded(
    http_response_coding_selection selection, http_response& response, blocking_pool& pool,
    const worker_handle& worker_value);

// Response compression policy. Fixed representations do not vary by coding;
// eligible representations are annotated before deciding between identity
// and an encoder. Encoder availability never changes Accept-Encoding parsing.
[[nodiscard]] http_response_compression_decision prepare_response_compression(
    const http_response_coding_selection& selection, http_known_method request_method,
    http_response& response, http_response_compression_source source_value,
    http_response_coding_availability availability);

}  // namespace ruvia::detail
