#pragma once

#include <cstddef>
#include <cstdint>
#include <utility>

#include "ruvia/http/http_known_method.h"
#include "ruvia/http/http_response.h"
#include "ruvia/http/http_response_trailer_section.h"

namespace ruvia {

enum class http_response_stream_framing : std::uint8_t {
    http1_known_length,
    http1_chunked,
    // RFC 9112 6.1: a server MUST NOT send Transfer-Encoding to a client that did
    // not indicate HTTP/1.1. An HTTP/1.0 stream therefore carries no chunk framing;
    // the body is delimited by the connection close (RFC 9112 6.3), so it also
    // announces Connection: close and the session shuts the socket afterwards.
    http1_close_delimited,
    http2_frames,
    http3_frames
};

enum class http_response_stream_kind : std::uint8_t { generic,
    sse };

// A trailer section is terminal message metadata, not an independently queued
// side channel. The caller declares whether this head commit is reserving a
// terminal trailer section so HTTP/2 can keep a content-forbidden response open
// for trailing HEADERS while HTTP/1 rejects an unavailable representation before
// emitting the response head.
enum class http_response_trailer_intent : std::uint8_t { none,
    present };

// Whether a validated trailer section commits the response to sending trailers.
// An empty section is not "no decision": it is the decision to send none, which
// the head must state before any body byte goes out.
[[nodiscard]] inline http_response_trailer_intent response_trailer_intent(
    const http_response_trailer_section& section) noexcept {
    return section.empty() ? http_response_trailer_intent::none : http_response_trailer_intent::present;
}

enum class http_response_stream_trailer_framing : std::uint8_t {
    unavailable,
    http1_chunked,
    http2_trailing_headers,
    http3_trailing_headers
};

// Authoritative phase immediately after the initial response head is submitted.
// trailers_only is distinct from body_open: HTTP/2 may carry trailing HEADERS
// after a HEAD/204-style response without allowing any DATA.
enum class http_response_stream_head_disposition : std::uint8_t { body_open,
    trailers_only,
    message_ended };

class http_response_stream_commit_plan final {
public:
    [[nodiscard]] http_status_code response_status() const noexcept {
        return body_plan_.response_status();
    }

    [[nodiscard]] http_response_stream_framing framing() const noexcept {
        return framing_;
    }

    [[nodiscard]] http_response_body_plan body_plan() const noexcept {
        return body_plan_;
    }

    [[nodiscard]] http_response_stream_trailer_framing trailer_framing() const noexcept {
        return trailer_framing_;
    }

    [[nodiscard]] http_response_stream_head_disposition head_disposition() const noexcept {
        return head_disposition_;
    }

    [[nodiscard]] bool trailer_intent_allowed() const noexcept {
        return !trailer_intent_present_ ||
               (trailer_framing_ != http_response_stream_trailer_framing::unavailable &&
                   !response_status().is_informational() &&
                   response_status() != http_status::no_content &&
                   response_status() != http_status::not_modified);
    }

private:
    friend http_response_stream_commit_plan plan_http_response_stream_commit(
        http_response_stream_framing, http_known_method, http_status_code, http_response_trailer_intent) noexcept;

    http_response_stream_commit_plan(http_response_stream_framing framing, http_response_body_plan body_plan,
        http_response_stream_trailer_framing trailer_framing,
        http_response_stream_head_disposition head_disposition, bool trailer_intent_present) noexcept
        : framing_(framing),
          body_plan_(body_plan),
          trailer_framing_(trailer_framing),
          head_disposition_(head_disposition),
          trailer_intent_present_(trailer_intent_present) {}

    http_response_stream_framing framing_{http_response_stream_framing::http2_frames};
    http_response_body_plan body_plan_;
    http_response_stream_trailer_framing trailer_framing_{http_response_stream_trailer_framing::unavailable};
    http_response_stream_head_disposition head_disposition_{http_response_stream_head_disposition::message_ended};
    bool trailer_intent_present_{false};
};

[[nodiscard]] inline http_response_stream_commit_plan plan_http_response_stream_commit(
    http_response_stream_framing framing, http_known_method request_method, http_status_code response_status,
    http_response_trailer_intent trailer_intent) noexcept {
    const auto body_plan = plan_http_response_body(request_method, response_status);
    if (framing == http_response_stream_framing::http2_frames || framing == http_response_stream_framing::http3_frames) {
        return http_response_stream_commit_plan(framing, body_plan,
            framing == http_response_stream_framing::http3_frames ? http_response_stream_trailer_framing::http3_trailing_headers : http_response_stream_trailer_framing::http2_trailing_headers,
            body_plan.body_suppressed() ? (trailer_intent == http_response_trailer_intent::present
                                                  ? http_response_stream_head_disposition::trailers_only
                                                  : http_response_stream_head_disposition::message_ended)
                                        : http_response_stream_head_disposition::body_open,
            trailer_intent == http_response_trailer_intent::present);
    }

    return http_response_stream_commit_plan(framing, body_plan,
        framing == http_response_stream_framing::http1_chunked && !body_plan.body_suppressed()
            ? http_response_stream_trailer_framing::http1_chunked
            : http_response_stream_trailer_framing::unavailable,
        body_plan.body_suppressed() ? http_response_stream_head_disposition::message_ended
                                    : http_response_stream_head_disposition::body_open,
        trailer_intent == http_response_trailer_intent::present);
}

// Preparation normalizes the initial response metadata to the supplied commit
// plan. Mutating the response afterward can invalidate that relationship; this
// object is not a permanent proof over a mutable response.
class http_response_stream_head final {
public:
    [[nodiscard]] http_response& response() & noexcept {
        return response_;
    }
    [[nodiscard]] http_response& response() && = delete;

    [[nodiscard]] const http_response& response() const& noexcept {
        return response_;
    }
    [[nodiscard]] const http_response& response() const&& = delete;

    [[nodiscard]] const http_response_stream_commit_plan& commit_plan() const& noexcept {
        return commit_plan_;
    }
    [[nodiscard]] const http_response_stream_commit_plan& commit_plan() const&& = delete;

private:
    friend http_response_stream_head prepare_http_response_stream_head(
        http_response, http_response_stream_kind, http_response_stream_commit_plan);

    http_response_stream_head(http_response response, http_response_stream_commit_plan commit_plan)
        : response_(std::move(response)),
          commit_plan_(commit_plan) {}

    http_response response_;
    http_response_stream_commit_plan commit_plan_;
};

[[nodiscard]] http_response_stream_head prepare_http_response_stream_head(
    http_response response, http_response_stream_kind kind, http_response_stream_commit_plan commit_plan);

}  // namespace ruvia
