#pragma once

#include <cstddef>
#include <cstdint>
#include <utility>

#include "ruvia/http/http_known_method.h"
#include "ruvia/http/http_response.h"
#include "ruvia/http/http_response_trailer_section.h"

namespace ruvia {

enum class http_response_stream_framing : std::uint8_t {
    http1_chunked,
    // RFC 9112 6.1: a server MUST NOT send Transfer-Encoding to a client that did
    // not indicate HTTP/1.1. An HTTP/1.0 stream therefore carries no chunk framing;
    // the body is delimited by the connection close (RFC 9112 6.3), so it also
    // announces Connection: close and the session shuts the socket afterwards.
    http1_close_delimited,
    http2_frames
};
// A trailer section is terminal message metadata, not an independently queued
// side channel. The caller declares whether this head commit is reserving a
// terminal trailer section. Trailers are optional metadata a recipient may
// discard (RFC 9110 6.5), so framing that cannot carry them (HTTP/1.0
// close-delimited, known length, or a suppressed body) drops them; only a
// status that can never carry content rejects the intent before the head.
enum class http_response_trailer_intent : std::uint8_t { none,
    present };

// Whether a validated trailer section commits the response to sending trailers.
// An empty section is not "no decision": it is the decision to send none, which
// the head must state before any body byte goes out.
[[nodiscard]] inline http_response_trailer_intent response_trailer_intent(
    const http_response_trailer_section& section) noexcept {
    return section.empty() ? http_response_trailer_intent::none : http_response_trailer_intent::present;
}

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

    // HTTP/1.1 requires chunked framing for trailers (RFC 9112 7.1.2).
    // HTTP/2 carries them in a trailing field section.
    [[nodiscard]] bool trailers_available() const noexcept {
        return (framing_ == http_response_stream_framing::http1_chunked && !body_plan_.body_suppressed()) ||
               framing_ == http_response_stream_framing::http2_frames;
    }

    [[nodiscard]] http_response_stream_head_disposition head_disposition() const noexcept {
        return head_disposition_;
    }

    // A status that can never carry content (1xx/204/205/304) has no message
    // body to terminate with a trailer section; asking for one is a handler
    // error. Every other unavailable framing discards the trailers instead.
    [[nodiscard]] bool status_permits_trailers() const noexcept {
        return !response_status().is_informational() && response_status() != http_status::no_content &&
               response_status() != http_status::reset_content && response_status() != http_status::not_modified;
    }

    [[nodiscard]] bool trailer_intent_allowed() const noexcept {
        return !trailer_intent_present_ || status_permits_trailers();
    }

private:
    friend http_response_stream_commit_plan plan_http_response_stream_commit(
        http_response_stream_framing, http_known_method, http_status_code, http_response_trailer_intent) noexcept;

    http_response_stream_commit_plan(http_response_stream_framing framing, http_response_body_plan body_plan,
        http_response_stream_head_disposition head_disposition, bool trailer_intent_present) noexcept
        : framing_(framing),
          body_plan_(body_plan),
          head_disposition_(head_disposition),
          trailer_intent_present_(trailer_intent_present) {}

    http_response_stream_framing framing_{http_response_stream_framing::http1_chunked};
    http_response_body_plan body_plan_;
    http_response_stream_head_disposition head_disposition_{http_response_stream_head_disposition::message_ended};
    bool trailer_intent_present_{false};
};

[[nodiscard]] inline http_response_stream_commit_plan plan_http_response_stream_commit(
    http_response_stream_framing framing, http_known_method request_method, http_status_code response_status,
    http_response_trailer_intent trailer_intent) noexcept {
    const auto body_plan = plan_http_response_body(request_method, response_status);
    if (framing == http_response_stream_framing::http2_frames) {
        return http_response_stream_commit_plan(framing, body_plan,
            body_plan.body_suppressed() ? (trailer_intent == http_response_trailer_intent::present
                                                  ? http_response_stream_head_disposition::trailers_only
                                                  : http_response_stream_head_disposition::message_ended)
                                        : http_response_stream_head_disposition::body_open,
            trailer_intent == http_response_trailer_intent::present);
    }

    return http_response_stream_commit_plan(framing, body_plan,
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
        http_response, http_response_stream_commit_plan);

    http_response_stream_head(http_response response, http_response_stream_commit_plan commit_plan)
        : response_(std::move(response)),
          commit_plan_(commit_plan) {}

    http_response response_;
    http_response_stream_commit_plan commit_plan_;
};

[[nodiscard]] http_response_stream_head prepare_http_response_stream_head(
    http_response response, http_response_stream_commit_plan commit_plan);
}  // namespace ruvia
