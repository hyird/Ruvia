#pragma once

#include <cstddef>
#include <exception>
#include <optional>
#include <string_view>
#include <variant>

#include "ruvia/http/http1_request_body_plan.h"
#include "ruvia/http/http1_request_connection_plan.h"
#include "ruvia/http/http1_request_parser.h"
#include "ruvia/http/http_accept_encoding.h"
#include "ruvia/http/http_parse_error.h"

namespace ruvia {

class http1_server_request_parse_failure;

namespace detail {
struct http1_request_parse_result_access;
}

enum class http1_server_request_parse_failure_source : std::uint8_t { request_line,
    message };

namespace detail {

struct http1_request_parse_result_access final {
    [[nodiscard]] static http1_request_parse_result need_more() noexcept {
        return http1_request_parse_result(http1_request_need_more());
    }

    [[nodiscard]] static http1_request_parse_result need_more(std::size_t required_total_bytes) noexcept {
        return http1_request_parse_result(http1_request_need_more(required_total_bytes));
    }

    // http_request is a fixed-size collection of borrowed views; the request and
    // framing plan are transferred into the result without allocating copies.
    [[nodiscard]] static http1_request_parse_result parsed(http_request request,
        http1_request_body_plan body_plan, std::string_view wire_body,
        std::size_t consumed_bytes) noexcept {
        return http1_request_parse_result(http1_parsed_request(
            std::move(request), std::move(body_plan), wire_body, consumed_bytes));
    }

    [[nodiscard]] static http1_request_parse_result failure(http_parse_error error,
        http1_request_parse_failure_source source_value) noexcept {
        return http1_request_parse_result(http1_request_parse_failure(error, source_value));
    }

    [[nodiscard]] static http1_request_parse_result failure(
        const ::ruvia::http1_server_request_parse_failure& failure) noexcept;
};

}  // namespace detail

class http1_server_need_request_head final {};

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

class http1_server_need_request_body final {
public:
    [[nodiscard]] constexpr std::size_t header_bytes() const noexcept {
        return header_bytes_;
    }

    [[nodiscard]] constexpr std::optional<std::size_t> required_total_bytes() const noexcept {
        return required_total_bytes_;
    }

private:
    friend class http1_server_request_parser;

    explicit constexpr http1_server_need_request_body(std::size_t header_bytes) noexcept
        : header_bytes_(header_bytes) {
        if (header_bytes_ == 0) {
            std::terminate();
        }
    }

    constexpr http1_server_need_request_body(
        std::size_t header_bytes, std::size_t required_total_bytes) noexcept
        : header_bytes_(header_bytes),
          required_total_bytes_(required_total_bytes) {
        if (header_bytes_ == 0 || (required_total_bytes_ && *required_total_bytes_ <= header_bytes_)) {
            std::terminate();
        }
    }

    std::size_t header_bytes_;
    std::optional<std::size_t> required_total_bytes_{};
};

class http1_server_request_message_ready final {
public:
    [[nodiscard]] constexpr std::size_t header_bytes() const noexcept {
        return header_bytes_;
    }

    [[nodiscard]] constexpr std::size_t message_bytes() const noexcept {
        return message_bytes_;
    }

private:
    friend class http1_server_request_parser;

    constexpr http1_server_request_message_ready(
        std::size_t header_bytes, std::size_t message_bytes) noexcept
        : header_bytes_(header_bytes),
          message_bytes_(message_bytes) {
        if (header_bytes_ == 0 || message_bytes_ < header_bytes_) {
            std::terminate();
        }
    }

    std::size_t header_bytes_;
    std::size_t message_bytes_;
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
    friend struct detail::http1_request_parse_result_access;

    explicit constexpr http1_server_request_parse_failure(http_parse_error error) noexcept
        : error_(error) {}

    http_parse_error error_;
};

namespace detail {
inline http1_request_parse_result http1_request_parse_result_access::failure(
    const ::ruvia::http1_server_request_parse_failure& failure) noexcept {
    const auto source_value = failure.source() == http1_server_request_parse_failure_source::request_line
                                  ? http1_request_parse_failure_source::request_line
                                  : http1_request_parse_failure_source::message;
    return http1_request_parse_result(http1_request_parse_failure(failure.error_, source_value));
}

}  // namespace detail

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

    [[nodiscard]] const http1_server_need_request_body* need_request_body() const& noexcept {
        return std::get_if<http1_server_need_request_body>(&progress_);
    }
    [[nodiscard]] const http1_server_need_request_body* need_request_body() const&& = delete;

    [[nodiscard]] const http1_server_request_message_ready* message_ready() const& noexcept {
        return std::get_if<http1_server_request_message_ready>(&progress_);
    }
    [[nodiscard]] const http1_server_request_message_ready* message_ready() const&& = delete;

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
    // Only this small progress value changes, so head/message/required/error
    // metadata exists solely in the alternative where it is meaningful.
    using progress = std::variant<http1_server_need_request_head, http1_server_request_head_ready,
        http1_server_need_request_body, http1_server_request_message_ready, http1_server_request_parse_failure>;

    progress progress_{http1_server_need_request_head{}};
    http_response_coding_qualities response_coding_qualities_;
};

class http1_server_request_parser final {
public:
    // Allocate the exact header descriptor block only after accepting the head.
    // The resource must outlive state; field text still borrows buffer.
    void parse_head(std::string_view buffer, http1_server_request_parse_state& state,
        std::size_t header_search_offset = 0, std::pmr::memory_resource* resource = nullptr) const;

    template <detail::http_temporary_owning_char_string buffer_type>
    void parse_head(buffer_type&&, http1_server_request_parse_state&, std::size_t = 0,
        std::pmr::memory_resource* = nullptr) const = delete;

    // Whole-message scanner used by the public sans-I/O API. It always advances
    // beyond request_head_ready to an unambiguous message/failure/need-more phase.
    [[nodiscard]] http1_server_request_parse_state parse_message(std::string_view buffer,
        std::pmr::memory_resource* resource = nullptr) const;

    template <detail::http_temporary_owning_char_string buffer_type>
    http1_server_request_parse_state parse_message(buffer_type&&, std::pmr::memory_resource* = nullptr) const = delete;

private:
    static void parse_request_head(std::string_view buffer, std::size_t header_search_offset,
        http1_server_request_parse_state& state, std::pmr::memory_resource* resource);
    static void parse_message_body(
        std::string_view buffer, http1_server_request_parse_state& state) noexcept;
};

}  // namespace ruvia
