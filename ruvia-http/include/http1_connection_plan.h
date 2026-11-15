#pragma once

#include <cstdint>

#include "borrowed_text.h"
#include "http_content_coding.h"
#include "http_message.h"

namespace ruvia {

// Whether the runtime consumed the complete request message before attempting
// to reuse its transport. This is a protocol lifecycle fact, not a product
// policy bool: RFC 9112 section 9.3 forbids reuse with unread request content.
enum class http1_request_body_consumption : std::uint8_t { complete,
    incomplete };

// Whether a connection is reused after a completed HTTP/1 exchange. Both the
// client request writer and the server response planner express the same wire
// decision with this single shared policy: allow_reuse keeps the connection
// poolable, close_after_response emits Connection: close and closes it.
enum class http1_close_policy : std::uint8_t {
    allow_reuse,
    close_after_response,
};

// How an HTTP/1 message delimits its content (RFC 9112 section 6.3).
enum class http1_framing_kind : std::uint8_t {
    // The message ends at its head: there is nothing to consume.
    no_content,
    // Content-Length delimits the content, which may be empty.
    known_length,
    // The final transfer coding is chunked; the zero-size chunk and trailer
    // section end the message.
    chunked,
    // The connection close ends the content.
    close_delimited,
};

// The one HTTP/1 framing decision, shared by request parsing, response
// parsing, response head planning and the message body decoder. Each kind
// carries only its own payload: a length for known_length, and the transfer
// codings applied before the framing for chunked and close_delimited.
class http1_message_framing final {
public:
    [[nodiscard]] static constexpr http1_message_framing no_content() noexcept {
        return http1_message_framing(http1_framing_kind::no_content, 0, {});
    }
    [[nodiscard]] static constexpr http1_message_framing known_length(std::uint64_t content_length) noexcept {
        return http1_message_framing(http1_framing_kind::known_length, content_length, {});
    }
    // transfer_codings lists the codings preceding the final chunked coding,
    // in the order they were applied.
    [[nodiscard]] static constexpr http1_message_framing chunked(http_content_codings transfer_codings = {}) noexcept {
        return http1_message_framing(http1_framing_kind::chunked, 0, transfer_codings);
    }
    [[nodiscard]] static constexpr http1_message_framing close_delimited(http_content_codings transfer_codings = {}) noexcept {
        return http1_message_framing(http1_framing_kind::close_delimited, 0, transfer_codings);
    }

    [[nodiscard]] constexpr http1_framing_kind kind() const noexcept {
        return kind_;
    }
    // Zero unless the kind is known_length.
    [[nodiscard]] constexpr std::uint64_t content_length() const noexcept {
        return content_length_;
    }
    // Empty unless the kind is chunked or close_delimited. A decoder removes
    // the framing first and then these codings in reverse order.
    [[nodiscard]] constexpr const http_content_codings& transfer_codings() const& noexcept RUVIA_LIFETIMEBOUND {
        return transfer_codings_;
    }
    const http_content_codings& transfer_codings() const&& = delete;

    // Whether bytes after the head belong to this message. Chunked framing
    // requires consuming the terminating zero chunk even when the decoded
    // content is empty.
    [[nodiscard]] constexpr bool requires_consumption() const noexcept {
        return kind_ == http1_framing_kind::known_length ? content_length_ != 0 : kind_ != http1_framing_kind::no_content;
    }

private:
    constexpr http1_message_framing(http1_framing_kind kind, std::uint64_t content_length, http_content_codings transfer_codings) noexcept :
        content_length_(content_length),
        transfer_codings_(transfer_codings),
        kind_(kind) {}

    std::uint64_t content_length_;
    http_content_codings transfer_codings_;
    http1_framing_kind kind_;
};

// Immutable request-side HTTP/1 persistence contract. Parsing establishes the
// version and initial close policy; later body-consumption policy may only close.
class http1_request_connection_plan final {
public:
    [[nodiscard]] static constexpr http1_request_connection_plan http11_close() noexcept {
        return {http_protocol_version::http11, http1_close_policy::close_after_response, false};
    }

    [[nodiscard]] constexpr http_protocol_version protocol_version() const noexcept {
        return version_;
    }
    [[nodiscard]] constexpr http1_close_policy close_policy() const noexcept {
        return close_policy_;
    }
    [[nodiscard]] constexpr http1_request_connection_plan require_close() const noexcept {
        return {version_, http1_close_policy::close_after_response, tunnel_request_};
    }

    // A CONNECT request. A 2xx final response turns the connection into a
    // tunnel; any other final response must close it, because a client may
    // already have sent tunnel bytes optimistically (RFC 9110 9.3.6) and they
    // must never be parsed as further requests.
    [[nodiscard]] constexpr bool tunnel_request() const noexcept {
        return tunnel_request_;
    }
    [[nodiscard]] constexpr http1_request_connection_plan with_tunnel_request() const noexcept {
        return {version_, close_policy_, true};
    }

private:
    friend constexpr http1_request_connection_plan plan_http1_request_connection(http_protocol_version, bool, bool) noexcept;

    constexpr http1_request_connection_plan(http_protocol_version version, http1_close_policy close_policy, bool tunnel_request) noexcept :
        version_(version),
        close_policy_(close_policy),
        tunnel_request_(tunnel_request) {}

    http_protocol_version version_;
    http1_close_policy close_policy_;
    bool tunnel_request_;
};

// The persistence a request's version and Connection options ask for (RFC 9112
// section 9.3): HTTP/1.1 persists unless `close` is present; HTTP/1.0 persists
// only with `keep_alive` and no `close`. `version` is http10 or http11.
[[nodiscard]] inline constexpr http1_request_connection_plan plan_http1_request_connection(http_protocol_version version, bool close, bool keep_alive) noexcept {
    const bool reuse = !close && (version == http_protocol_version::http11 || keep_alive);
    return {version, reuse ? http1_close_policy::allow_reuse : http1_close_policy::close_after_response, false};
}

[[nodiscard]] inline constexpr http1_request_connection_plan apply_request_body_consumption(http1_request_connection_plan plan, http1_request_body_consumption consumption) noexcept {
    return consumption == http1_request_body_consumption::complete ? plan : plan.require_close();
}

}  // namespace ruvia
