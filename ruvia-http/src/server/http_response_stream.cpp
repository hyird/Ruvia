#include "ruvia/http/http_response_stream.h"

#include <cstddef>
#include <stdexcept>
#include <utility>

#include "ruvia/http/detail/response/http_response_header_state.h"
#include "ruvia/http/detail/server/http_response_head_policy.h"

namespace ruvia {

[[nodiscard]] http_response_stream_head prepare_http_response_stream_head(
    http_response response, http_response_stream_kind kind, http_response_stream_commit_plan commit_plan) {
    if (response.status() != commit_plan.response_status()) {
        throw std::invalid_argument("response stream commit plan status does not match response");
    }
    if (!commit_plan.trailer_intent_allowed()) {
        throw std::invalid_argument("response stream status does not permit trailers");
    }
    const auto framing = commit_plan.framing();
    const auto body_plan = commit_plan.body_plan();
    const bool writer_owns_http1_known_length =
        framing == http_response_stream_framing::http1_known_length && body_plan.auto_content_length_allowed();
    const bool writer_owns_http1_chunked = framing == http_response_stream_framing::http1_chunked &&
                                           body_plan.transfer_encoding_allowed() &&
                                           !body_plan.body_suppressed();

    // Keep the prepared response metadata consistent with the wire plan. The
    // framework's chunk writer is the only Transfer-Encoding producer; an
    // HTTP/1.0 close-delimited body cannot retain either framing field. HEAD/304
    // may retain Content-Length metadata because their body is suppressed.
    if (writer_owns_http1_known_length || writer_owns_http1_chunked) {
        response.remove_header("Content-Length");
    }
    if (framing == http_response_stream_framing::http1_known_length ||
        framing == http_response_stream_framing::http1_chunked ||
        framing == http_response_stream_framing::http1_close_delimited) {
        response.remove_header("Transfer-Encoding");
    }
    if (framing == http_response_stream_framing::http1_close_delimited && !body_plan.body_suppressed()) {
        response.remove_header("Content-Length");
    }
    // Without a chunked body the trailer section is discarded, so the head must
    // not announce trailer fields that will never follow (RFC 9110 6.6.2).
    if (!commit_plan.trailers_available()) {
        response.remove_header("Trailer");
    }

    const bool needs_sse_content_type = kind == http_response_stream_kind::sse &&
                                        !detail::response_has_known_header(response, detail::response_header_content_type);
    const bool needs_http1_chunked = writer_owns_http1_chunked && !detail::response_has_known_header(response,
                                                                      detail::response_header_transfer_encoding);
    const bool needs_sse_cache_control =
        kind == http_response_stream_kind::sse &&
        body_plan.transfer_encoding_allowed() &&
        !detail::response_has_known_header(response, detail::response_header_cache_control);
    const auto additional_headers = static_cast<std::size_t>(needs_sse_content_type) +
                                    static_cast<std::size_t>(needs_http1_chunked) +
                                    static_cast<std::size_t>(needs_sse_cache_control);
    if (additional_headers != 0) {
        response.reserve_headers(response.headers().size() + additional_headers);
    }

    if (needs_sse_content_type) {
        response.header_stable_view("Content-Type", "text/event-stream");
    }
    if (writer_owns_http1_chunked) {
        response.header_stable_view("Transfer-Encoding", "chunked");
    }
    if (needs_sse_cache_control) {
        // Like Content-Type, a handler's own Cache-Control (for example the SSE
        // "no-cache") is honored; needs_sse_cache_control excludes it.
        response.header_stable_view("Cache-Control", "no-store");
    }

    return http_response_stream_head(std::move(response), commit_plan);
}

}  // namespace ruvia
