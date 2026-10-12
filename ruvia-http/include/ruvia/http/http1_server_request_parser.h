#pragma once

#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory_resource>
#include <string_view>
#include <variant>

#include "ruvia/http/detail/util/borrowed_view.h"
#include "ruvia/http/http1_request_body_plan.h"
#include "ruvia/http/http1_request_connection_plan.h"
#include "ruvia/http/http_accept_encoding.h"
#include "ruvia/http/http_parse_error.h"
#include "ruvia/http/http_request.h"

namespace ruvia {

enum class http1_server_request_parse_failure_source : std::uint8_t { request_line,
    message };

// Cleartext listeners can silently discard inputs whose request-line failure
// shows the peer was not speaking HTTP (for example, TLS bytes on an HTTP
// port): the request-line starts with a byte that cannot begin a method token,
// or its first CRLF-terminated line carries no HTTP/ version token.
[[nodiscard]] bool should_drop_invalid_cleartext_http1_input(
    std::string_view buffer, http1_server_request_parse_failure_source source_value) noexcept;

class http1_server_need_request_head final {
private:
    friend class http1_server_request_parser;

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
    friend class http1_server_request_parser;

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
    friend class http1_server_request_parser;

    explicit constexpr http1_server_request_parse_failure(http_parse_error error) noexcept
        : error_(error) {}

    http_parse_error error_;
};

class http1_server_request_parse_state final {
public:
    http1_server_request_parse_state() noexcept;

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

    http_request request_;
    http1_request_body_plan body_plan_{http1_request_body_plan(http_request_expectations{})};
    http1_request_connection_plan connection_plan_{http1_request_connection_plan::http11_close()};

    // Parsed once from the request head. Representation policy consumes this
    // complete client preference later, without rescanning Accept-Encoding.
    [[nodiscard]] http_response_coding_selection_result response_coding_selection() const noexcept {
        return http_response_coding_selection::select(response_coding_qualities_);
    }

private:
    friend class http1_server_request_parser;

    // The compact request owns its descriptor block across parse attempts.
    // Only this small progress value changes, so scan/head/error metadata
    // exists solely in the alternative where it is meaningful.
    using progress = std::variant<http1_server_need_request_head, http1_server_request_head_ready,
        http1_server_request_parse_failure>;

    progress progress_{http1_server_need_request_head{}};
    http_response_coding_qualities response_coding_qualities_;
};

class http1_server_request_parser final {
public:
    // Allocate the exact header descriptor block only after accepting the head.
    // Only state.request_ draws from resource, which must outlive it until the
    // request is reset or reparsed; field text still borrows buffer. Empty
    // lines before the request-line are ignored (RFC 9112 section 2.2) within
    // max_http_header_bytes and are counted in header_bytes(). Bytes before
    // header_search_offset were examined by an earlier call on the same state
    // and buffer prefix. An incomplete head already fails with a request_line
    // source once its request-line is CRLF-terminated or cannot start a method.
    void parse_head(std::string_view buffer, http1_server_request_parse_state& state,
        std::size_t header_search_offset = 0, std::pmr::memory_resource* resource = nullptr) const;

    template <detail::http_temporary_owning_char_string buffer_type>
    void parse_head(buffer_type&&, http1_server_request_parse_state&, std::size_t = 0,
        std::pmr::memory_resource* = nullptr) const = delete;
};

}  // namespace ruvia
