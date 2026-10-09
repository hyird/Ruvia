#pragma once

#include <cstdint>
#include <type_traits>
#include <variant>

#include "ruvia/http/detail/server/http_response_write_plan.h"
#include "ruvia/http/http1_request_connection_plan.h"
#include "ruvia/http/http_protocol_version.h"

namespace ruvia {

class http1_response_head_plan;
class http1_buffered_response_plan;

// A complete buffered representation is length-delimited by the response writer.
class http1_buffered_response_head final {
public:
    [[nodiscard]] constexpr std::uint64_t content_length() const noexcept {
        return content_length_;
    }

private:
    friend class http1_response_head_plan;

    explicit constexpr http1_buffered_response_head(std::uint64_t content_length) noexcept
        : content_length_(content_length) {}

    std::uint64_t content_length_{0};
};

// The response writer owns canonical Transfer-Encoding: chunked and the runtime
// emits the matching chunk frames. Application headers cannot redefine framing.
class http1_chunked_response_stream_head final {
private:
    friend class http1_response_head_plan;

    constexpr http1_chunked_response_stream_head() noexcept = default;
};

// A streamed response with an exact representation length. The runtime writes
// chunks incrementally, while the HTTP plan owns the one canonical
// Content-Length field and permits connection reuse on HTTP/1.0 and HTTP/1.1.
class http1_known_length_response_stream_head final {
public:
    [[nodiscard]] constexpr std::uint64_t content_length() const noexcept {
        return content_length_;
    }

private:
    friend class http1_response_head_plan;

    explicit constexpr http1_known_length_response_stream_head(std::uint64_t content_length) noexcept
        : content_length_(content_length) {}

    std::uint64_t content_length_{0};
};

// The response has no declared message-body length. When content is allowed, the
// runtime closes the connection to delimit it; application Content-Length and
// Transfer-Encoding fields therefore cannot survive into the wire head.
class http1_close_delimited_response_stream_head final {
private:
    friend class http1_response_head_plan;

    constexpr http1_close_delimited_response_stream_head() noexcept = default;
};

// Final HTTP/1 head framing is an exclusive protocol value, not the old
// "suppress automatic Content-Length" boolean. The body plan keeps method/status
// content semantics attached to the exact wire-framing alternative selected by
// the HTTP/1 planner.
class http1_response_head_plan final {
public:
    [[nodiscard]] constexpr const http1_buffered_response_head* buffered() const& noexcept {
        return std::get_if<http1_buffered_response_head>(&framing_);
    }
    [[nodiscard]] constexpr const http1_buffered_response_head* buffered() const&& = delete;

    [[nodiscard]] constexpr const http1_chunked_response_stream_head* chunked_stream() const& noexcept {
        return std::get_if<http1_chunked_response_stream_head>(&framing_);
    }
    [[nodiscard]] constexpr const http1_chunked_response_stream_head* chunked_stream() const&& = delete;

    [[nodiscard]] constexpr const http1_known_length_response_stream_head* known_length_stream()
        const& noexcept {
        return std::get_if<http1_known_length_response_stream_head>(&framing_);
    }
    [[nodiscard]] constexpr const http1_known_length_response_stream_head* known_length_stream() const&& =
        delete;

    [[nodiscard]] constexpr const http1_close_delimited_response_stream_head* close_delimited_stream()
        const& noexcept {
        return std::get_if<http1_close_delimited_response_stream_head>(&framing_);
    }
    [[nodiscard]] constexpr const http1_close_delimited_response_stream_head* close_delimited_stream()
        const&& = delete;

    [[nodiscard]] constexpr http_response_body_plan body_plan() const noexcept {
        return body_plan_;
    }

    [[nodiscard]] constexpr http_protocol_version protocol_version() const noexcept {
        return protocol_version_;
    }

private:
    friend http1_buffered_response_plan get_http1_buffered_response_plan(
        http_buffered_response_write_plan, http1_request_connection_plan) noexcept;
    friend constexpr http1_response_head_plan http1_known_length_response_stream_head_plan(
        http_response_body_plan, http1_request_connection_plan, std::uint64_t) noexcept;
    friend constexpr http1_response_head_plan http1_chunked_response_stream_head_plan(
        http_response_body_plan, http1_request_connection_plan) noexcept;
    friend constexpr http1_response_head_plan http1_close_delimited_response_stream_head_plan(
        http_response_body_plan, http1_request_connection_plan) noexcept;

    using framing_type = std::variant<http1_buffered_response_head, http1_known_length_response_stream_head,
        http1_chunked_response_stream_head, http1_close_delimited_response_stream_head>;

    [[nodiscard]] static constexpr framing_type buffered_framing(std::uint64_t content_length) noexcept {
        return framing_type(http1_buffered_response_head(content_length));
    }

    [[nodiscard]] static constexpr framing_type chunked_stream_framing() noexcept {
        return framing_type(http1_chunked_response_stream_head());
    }

    [[nodiscard]] static constexpr framing_type known_length_stream_framing(
        std::uint64_t content_length) noexcept {
        return framing_type(http1_known_length_response_stream_head(content_length));
    }

    [[nodiscard]] static constexpr framing_type close_delimited_stream_framing() noexcept {
        return framing_type(http1_close_delimited_response_stream_head());
    }

    constexpr http1_response_head_plan(http_response_body_plan body_plan,
        http_protocol_version protocol_version, framing_type framing) noexcept
        : body_plan_(body_plan),
          protocol_version_(protocol_version),
          framing_(framing) {}

    http_response_body_plan body_plan_;
    http_protocol_version protocol_version_;
    framing_type framing_;
};

[[nodiscard]] constexpr http1_response_head_plan http1_known_length_response_stream_head_plan(
    http_response_body_plan body_plan, http1_request_connection_plan connection_plan,
    std::uint64_t content_length) noexcept {
    return http1_response_head_plan(body_plan, connection_plan.protocol_version(),
        http1_response_head_plan::known_length_stream_framing(content_length));
}

[[nodiscard]] constexpr http1_response_head_plan http1_chunked_response_stream_head_plan(
    http_response_body_plan body_plan, http1_request_connection_plan connection_plan) noexcept {
    return http1_response_head_plan(
        body_plan, connection_plan.protocol_version(), http1_response_head_plan::chunked_stream_framing());
}

[[nodiscard]] constexpr http1_response_head_plan http1_close_delimited_response_stream_head_plan(
    http_response_body_plan body_plan, http1_request_connection_plan connection_plan) noexcept {
    return http1_response_head_plan(body_plan, connection_plan.protocol_version(),
        http1_response_head_plan::close_delimited_stream_framing());
}

// The runtime writes one inseparable HTTP/1 buffered response contract. The
// embedded head owns method/status/body semantics and representation length
// exactly once; direct fact access avoids restoring a parallel write-plan copy.
class http1_buffered_response_plan final {
public:
    [[nodiscard]] constexpr http_response_body_plan body_plan() const noexcept {
        return head_plan_.body_plan();
    }

    [[nodiscard]] http_status_code response_status() const noexcept {
        return body_plan().response_status();
    }

    [[nodiscard]] constexpr std::uint64_t content_length() const noexcept {
        return head_plan_.buffered()->content_length();
    }

    [[nodiscard]] bool send_body() const noexcept {
        return !body_plan().body_suppressed() && content_length() != 0;
    }

    [[nodiscard]] constexpr const http1_response_head_plan& head_plan() const& noexcept {
        return head_plan_;
    }
    [[nodiscard]] constexpr const http1_response_head_plan& head_plan() const&& = delete;

private:
    friend http1_buffered_response_plan get_http1_buffered_response_plan(
        http_buffered_response_write_plan, http1_request_connection_plan) noexcept;

    explicit constexpr http1_buffered_response_plan(http1_response_head_plan head_plan) noexcept
        : head_plan_(head_plan) {}

    http1_response_head_plan head_plan_;
};

static_assert(std::is_trivially_copyable_v<http1_buffered_response_plan>);
static_assert(sizeof(http1_buffered_response_plan) == sizeof(http1_response_head_plan));

[[nodiscard]] inline http1_buffered_response_plan get_http1_buffered_response_plan(
    http_buffered_response_write_plan write_plan, http1_request_connection_plan connection_plan) noexcept {
    return http1_buffered_response_plan(
        http1_response_head_plan(write_plan.body_plan(), connection_plan.protocol_version(),
            http1_response_head_plan::buffered_framing(write_plan.content_length())));
}

}  // namespace ruvia
