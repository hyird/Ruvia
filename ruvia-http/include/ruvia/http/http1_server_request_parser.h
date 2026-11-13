#pragma once

#include <cstddef>
#include <cstdint>
#include <exception>
#include <optional>
#include <string_view>
#include <utility>
#include <variant>

#include "ruvia/http/borrowed_text.h"
#include "ruvia/http/http1_chunked_body_decoder.h"
#include "ruvia/http/http_field_section.h"
#include "ruvia/http/http_header.h"
#include "ruvia/http/http_protocol_error.h"
#include "ruvia/http/http_request.h"
namespace ruvia {

class http1_server_request_parse_state;

// Whether the runtime consumed the complete request message before attempting
// to reuse its transport. This is a protocol lifecycle fact, not a product
// policy bool: RFC 9112 section 9.3 forbids reuse with unread request content.
enum class http1_request_body_consumption : std::uint8_t { complete,
    incomplete };

class http1_request_without_body final {
private:
    friend class http1_request_body_plan;
    constexpr http1_request_without_body() noexcept = default;
};

class http1_known_length_request_body final {
public:
    [[nodiscard]] constexpr std::size_t content_length() const noexcept {
        return content_length_;
    }

private:
    friend class http1_request_body_plan;

    explicit constexpr http1_known_length_request_body(std::size_t content_length) noexcept
        : content_length_(content_length) {}

    std::size_t content_length_;
};

class http1_chunked_request_body final {
public:
    [[nodiscard]] const http_transfer_codings& transfer_codings() const& noexcept RUVIA_LIFETIMEBOUND {
        return transfer_codings_;
    }
    const http_transfer_codings& transfer_codings() const&& = delete;

private:
    friend class http1_request_body_plan;

    explicit http1_chunked_request_body(http_transfer_codings transfer_codings)
        : transfer_codings_(std::move(transfer_codings)) {}

    http_transfer_codings transfer_codings_;
};

// Immutable framing contract produced only by the HTTP/1 parser and consumed
// by any runtime driver. Content-Length belongs only to the known-length
// alternative; transfer-coding order belongs only to the final-chunked
// alternative. A caller cannot synthesize a plan that bypasses wire validation.
class http1_request_body_plan final {
public:
    [[nodiscard]] constexpr const http1_request_without_body* without_body() const& noexcept RUVIA_LIFETIMEBOUND {
        return std::get_if<http1_request_without_body>(&framing_);
    }
    [[nodiscard]] constexpr const http1_request_without_body* without_body() const&& = delete;

    [[nodiscard]] constexpr const http1_known_length_request_body* known_length() const& noexcept RUVIA_LIFETIMEBOUND {
        return std::get_if<http1_known_length_request_body>(&framing_);
    }
    [[nodiscard]] constexpr const http1_known_length_request_body* known_length() const&& = delete;

    [[nodiscard]] constexpr const http1_chunked_request_body* chunked() const& noexcept RUVIA_LIFETIMEBOUND {
        return std::get_if<http1_chunked_request_body>(&framing_);
    }
    [[nodiscard]] constexpr const http1_chunked_request_body* chunked() const&& = delete;

    // Chunked framing requires consuming the terminating zero chunk even when
    // the decoded content is empty.
    [[nodiscard]] constexpr bool requires_consumption() const noexcept {
        if (const auto* known = known_length()) {
            return known->content_length() != 0;
        }
        return chunked() != nullptr;
    }

    // The consumption a response must record when the runtime never read the
    // request body: incomplete only if there is anything to read.
    [[nodiscard]] constexpr http1_request_body_consumption consumption_if_unread() const noexcept {
        return requires_consumption() ? http1_request_body_consumption::incomplete
                                      : http1_request_body_consumption::complete;
    }

    [[nodiscard]] http_request_expectations expectations() const noexcept {
        return expectations_;
    }

    [[nodiscard]] http_server_expectation_plan expectation_plan(
        http_unsupported_expectation_policy unsupported_policy) const noexcept {
        return expectations_.server_plan(requires_consumption()
                                             ? http_request_content_indication::will_follow
                                             : http_request_content_indication::no_content,
            unsupported_policy);
    }

private:
    friend class http1_server_request_parse_state;

    using framing_type =
        std::variant<http1_request_without_body, http1_known_length_request_body, http1_chunked_request_body>;

    explicit http1_request_body_plan(http_request_expectations expectations) noexcept
        : http1_request_body_plan(framing_type(http1_request_without_body()), expectations) {}

    http1_request_body_plan(std::size_t content_length, http_request_expectations expectations) noexcept
        : http1_request_body_plan(framing_type(http1_known_length_request_body(content_length)), expectations) {}

    http1_request_body_plan(
        http_transfer_codings transfer_codings, http_request_expectations expectations)
        : http1_request_body_plan(framing_type(http1_chunked_request_body(std::move(transfer_codings))), expectations) {}

    http1_request_body_plan(framing_type framing, http_request_expectations expectations)
        : framing_(std::move(framing)),
          expectations_(expectations) {}

    framing_type framing_;
    http_request_expectations expectations_;
};

}  // namespace ruvia

namespace ruvia {

// Whether a connection is reused after a completed HTTP/1 exchange. Both the
// client request writer and the server response planner express the same wire
// decision with this single shared policy: allow_reuse keeps the connection
// poolable, close_after_response emits Connection: close and closes it.
enum class http1_close_policy : std::uint8_t {
    allow_reuse,
    close_after_response,
};
// Immutable request-side HTTP/1 persistence contract. Parsing establishes the
// version and initial disposition; later body-consumption policy may only close.
class http1_request_connection_plan final {
public:
    [[nodiscard]] static constexpr http1_request_connection_plan http11_close() noexcept {
        return {http_protocol_version::http11, http1_close_policy::close_after_response, false};
    }

    [[nodiscard]] constexpr http_protocol_version protocol_version() const noexcept {
        return version_;
    }
    [[nodiscard]] constexpr http1_close_policy disposition() const noexcept {
        return disposition_;
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
        return {version_, disposition_, true};
    }

private:
    friend constexpr http1_request_connection_plan plan_http10_request_connection(bool, bool) noexcept;
    friend constexpr http1_request_connection_plan plan_http11_request_connection(bool) noexcept;

    constexpr http1_request_connection_plan(http_protocol_version version,
        http1_close_policy disposition, bool tunnel_request) noexcept
        : version_(version),
          disposition_(disposition),
          tunnel_request_(tunnel_request) {}

    http_protocol_version version_;
    http1_close_policy disposition_;
    bool tunnel_request_;
};

[[nodiscard]] inline constexpr http1_request_connection_plan plan_http10_request_connection(
    bool close, bool keep_alive) noexcept {
    return {http_protocol_version::http10,
        !close && keep_alive ? http1_close_policy::allow_reuse : http1_close_policy::close_after_response, false};
}

[[nodiscard]] inline constexpr http1_request_connection_plan plan_http11_request_connection(
    bool close) noexcept {
    return {http_protocol_version::http11,
        close ? http1_close_policy::close_after_response : http1_close_policy::allow_reuse, false};
}

[[nodiscard]] inline constexpr http1_request_connection_plan apply_request_body_consumption(
    http1_request_connection_plan plan, http1_request_body_consumption consumption) noexcept {
    return consumption == http1_request_body_consumption::complete ? plan : plan.require_close();
}

}  // namespace ruvia

namespace ruvia {

enum class http1_server_request_parse_failure_source : std::uint8_t { request_line,
    message };

// Cleartext listeners can silently discard inputs whose request-line failure
// shows the peer was not speaking HTTP (for example, TLS bytes on an HTTP
// port). Input whose first line (after ignored empty lines; the available
// bytes while no CRLF has arrived) has a whitespace-separated word naming an
// HTTP version ("HTTP/" in any case, or token "/" DIGITs "." DIGITs) is HTTP
// and is answered with 400 (RFC 9112 sections 2.2 and 3). Otherwise input is
// dropped once its first line is CRLF-terminated, or at once when its first
// byte cannot begin a method token.
[[nodiscard]] bool should_drop_invalid_cleartext_http1_input(
    std::string_view buffer, http1_server_request_parse_failure_source source_value) noexcept;

class http1_server_need_request_head final {
private:
    friend class http1_server_request_parse_state;

    // Resumable scan progress: leading empty-line bytes already skipped and,
    // once accepted, the end of the request-line. Each input byte is thereby
    // classified once across repeated parse_head calls.
    std::size_t leading_bytes_{0};
    std::size_t request_line_end_{0};
};

class http1_server_request_head_ready final {
public:
    [[nodiscard]] constexpr std::size_t header_bytes() const noexcept {
        return header_bytes_;
    }

private:
    friend class http1_server_request_parse_state;

    explicit constexpr http1_server_request_head_ready(std::size_t header_bytes) noexcept
        : header_bytes_(header_bytes) {
        if (header_bytes_ == 0) {
            std::terminate();
        }
    }

    std::size_t header_bytes_;
};

class http1_server_request_parse_failure final {
public:
    [[nodiscard]] http_protocol_error protocol_error() const noexcept {
        return http_parse_protocol_error(error_);
    }

    [[nodiscard]] constexpr http1_server_request_parse_failure_source source() const noexcept {
        return error_ == http_parse_error::invalid_request_line ||
                       error_ == http_parse_error::unsupported_http_version
                   ? http1_server_request_parse_failure_source::request_line
                   : http1_server_request_parse_failure_source::message;
    }

private:
    friend class http1_server_request_parse_state;

    explicit constexpr http1_server_request_parse_failure(http_parse_error error) noexcept
        : error_(error) {}

    http_parse_error error_;
};

class http1_server_request_parse_state final {
public:
    http1_server_request_parse_state() noexcept;

    // 分配失败以 std::bad_alloc 传出且不发布 head_ready/成功状态；释放资源后可用同一 buffer 与 offset 重试，但部分元数据可能残留。
    void parse_head(std::string_view buffer, std::size_t header_search_offset = 0);

    template <detail::http_temporary_owning_char_string buffer_type>
    void parse_head(buffer_type&&, std::size_t = 0) = delete;
    [[nodiscard]] const http1_server_need_request_head* need_request_head() const& noexcept {
        return std::get_if<http1_server_need_request_head>(&progress_);
    }
    [[nodiscard]] const http1_server_need_request_head* need_request_head() const&& = delete;

    [[nodiscard]] const http1_server_request_head_ready* head_ready() const& noexcept {
        return std::get_if<http1_server_request_head_ready>(&progress_);
    }
    [[nodiscard]] const http1_server_request_head_ready* head_ready() const&& = delete;

    [[nodiscard]] const http1_server_request_parse_failure* failure() const& noexcept {
        return std::get_if<http1_server_request_parse_failure>(&progress_);
    }
    [[nodiscard]] const http1_server_request_parse_failure* failure() const&& = delete;

    // The request, body plan and connection plan parse_head() derived from
    // its last input. A failure keeps the accepted request version and a
    // connection plan that requires close; an incomplete head keeps an empty
    // request.
    [[nodiscard]] const http_request& request() const& noexcept RUVIA_LIFETIMEBOUND {
        return request_;
    }
    const http_request& request() const&& = delete;
    [[nodiscard]] const http1_request_body_plan& body_plan() const& noexcept RUVIA_LIFETIMEBOUND {
        return body_plan_;
    }
    const http1_request_body_plan& body_plan() const&& = delete;
    [[nodiscard]] const http1_request_connection_plan& connection_plan() const& noexcept RUVIA_LIFETIMEBOUND {
        return connection_plan_;
    }
    const http1_request_connection_plan& connection_plan() const&& = delete;

    // Each extraction moves one result out and leaves the default value
    // behind; the state then serves only parse_head() or destruction. Taking
    // the request releases its descriptor storage.
    [[nodiscard]] http_request take_request() && noexcept;
    [[nodiscard]] http1_request_body_plan take_body_plan() && noexcept;
    [[nodiscard]] http1_request_connection_plan take_connection_plan() && noexcept;

private:
    http_request request_;
    http1_request_body_plan body_plan_{http1_request_body_plan(http_request_expectations{})};
    http1_request_connection_plan connection_plan_{http1_request_connection_plan::http11_close()};
    // The compact request owns its descriptor block across parse attempts.
    // Only this small progress value changes, so scan/head/error metadata
    // exists solely in the alternative where it is meaningful.
    using progress = std::variant<http1_server_need_request_head, http1_server_request_head_ready,
        http1_server_request_parse_failure>;

    progress progress_{http1_server_need_request_head{}};
};

}  // namespace ruvia
