#pragma once

#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory_resource>
#include <optional>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

#include "ruvia/http/detail/util/borrowed_view.h"
#include "ruvia/http/http1_request_body_plan.h"
#include "ruvia/http/http_parse_error.h"
#include "ruvia/http/http_request.h"

namespace ruvia {

namespace detail {

struct http1_request_parse_result_access;

}  // namespace detail

// The parser cannot determine an exact final size while the header section or
// chunked body is incomplete. A Content-Length body does provide that exact
// total, allowing an I/O owner to reserve once without overloading a
// "consumed" field with two unrelated meanings.
class http1_request_need_more final {
public:
    [[nodiscard]] constexpr std::optional<std::size_t> required_total_bytes() const noexcept {
        return required_total_bytes_;
    }

private:
    friend struct detail::http1_request_parse_result_access;

    constexpr http1_request_need_more() noexcept = default;

    explicit constexpr http1_request_need_more(std::size_t required_total_bytes) noexcept
        : required_total_bytes_(required_total_bytes) {
        if (required_total_bytes_ && *required_total_bytes_ == 0) {
            std::terminate();
        }
    }

    std::optional<std::size_t> required_total_bytes_{};
};

// One completely framed HTTP/1 request. Owns its compact header descriptor
// block in the parse resource. Field strings and body borrow the input passed
// to parse() and remain valid only while those bytes remain alive and unmoved.
class http1_parsed_request final {
public:
    [[nodiscard]] const http_request& request() const& noexcept {
        return request_;
    }
    [[nodiscard]] const http_request& request() const&& = delete;

    [[nodiscard]] const http1_request_body_plan& body_plan() const& noexcept {
        return body_plan_;
    }
    [[nodiscard]] const http1_request_body_plan& body_plan() const&& = delete;

    // Exact wire bytes after the header section and before the next message.
    // For Content-Length this is the payload. For chunked framing it retains
    // every chunk-size line, delimiter, and trailer field so a sans-I/O owner
    // can drive the shared decoder without the parser silently dropping data.
    [[nodiscard]] std::string_view wire_body() const noexcept {
        return wire_body_;
    }

    [[nodiscard]] constexpr std::size_t consumed_bytes() const noexcept {
        return consumed_bytes_;
    }

private:
    friend struct detail::http1_request_parse_result_access;

    // Transfer the descriptor block; header text continues to borrow input.
    http1_parsed_request(http_request request, http1_request_body_plan body_plan,
        std::string_view wire_body, std::size_t consumed_bytes) noexcept
        : request_(std::move(request)),
          body_plan_(std::move(body_plan)),
          wire_body_(wire_body),
          consumed_bytes_(consumed_bytes) {}

    http_request request_;
    http1_request_body_plan body_plan_;
    std::string_view wire_body_;
    std::size_t consumed_bytes_{0};
};

static_assert(std::is_nothrow_move_constructible_v<http1_parsed_request>);

enum class http1_request_parse_failure_source : std::uint8_t { request_line,
    message };

class http1_request_parse_failure final {
public:
    [[nodiscard]] http_protocol_error protocol_error() const noexcept {
        return http_parse_protocol_error(error_);
    }

    [[nodiscard]] constexpr http1_request_parse_failure_source source() const noexcept {
        return source_;
    }

private:
    friend struct detail::http1_request_parse_result_access;

    explicit constexpr http1_request_parse_failure(
        http_parse_error error, http1_request_parse_failure_source source_value) noexcept
        : error_(error),
          source_(source_value) {}

    http_parse_error error_;
    http1_request_parse_failure_source source_;
};

// Cleartext listeners can silently discard inputs that failed before a valid
// HTTP-version token was recognized (for example, TLS bytes on an HTTP port).
[[nodiscard]] bool should_drop_invalid_cleartext_http1_input(
    std::string_view buffer, http1_request_parse_failure_source source_value) noexcept;

// A discriminated parse outcome. Request data exists only in http1_parsed_request,
// an error exists only in http1_request_parse_failure, and input sizing exists only
// in http1_request_need_more.
// Callers therefore cannot read a default request after an error or mistake a
// required buffer size for bytes already consumed.
class http1_request_parse_result final {
public:
    [[nodiscard]] const http1_request_need_more* need_more() const& noexcept {
        return std::get_if<http1_request_need_more>(&state_);
    }
    const http1_request_need_more* need_more() const&& = delete;

    [[nodiscard]] const http1_parsed_request* parsed() const& noexcept {
        return std::get_if<http1_parsed_request>(&state_);
    }
    const http1_parsed_request* parsed() const&& = delete;

    [[nodiscard]] const http1_request_parse_failure* failure() const& noexcept {
        return std::get_if<http1_request_parse_failure>(&state_);
    }
    const http1_request_parse_failure* failure() const&& = delete;

private:
    friend struct detail::http1_request_parse_result_access;

    explicit http1_request_parse_result(http1_request_need_more state_value) noexcept
        : state_(state_value) {}

    explicit http1_request_parse_result(http1_parsed_request state_value) noexcept
        : state_(std::move(state_value)) {}

    explicit http1_request_parse_result(http1_request_parse_failure state_value) noexcept
        : state_(state_value) {}

    std::variant<http1_request_need_more, http1_parsed_request, http1_request_parse_failure> state_;
};

static_assert(std::is_nothrow_move_constructible_v<http1_request_parse_result>);

struct http1_request_parse_options final {
    // Must outlive the result. nullptr uses the default PMR resource.
    std::pmr::memory_resource* resource_{nullptr};
};

// Stateless whole-message scanner: field text is zero-copy, descriptors are
// allocated in the parse resource. Allocation failures propagate; syntax and
// framing failures use the typed result. No transfer decoding or input mutation.
class http1_request_parser final {
public:
    [[nodiscard]] http1_request_parse_result parse(std::string_view buffer,
        http1_request_parse_options options = {}) const;

    template <detail::http_temporary_owning_char_string buffer_type>
    http1_request_parse_result parse(buffer_type&&, http1_request_parse_options = {}) const = delete;
};

}  // namespace ruvia
