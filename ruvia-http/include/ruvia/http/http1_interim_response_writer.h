#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <variant>

#include "ruvia/http/http_interim_response.h"

namespace ruvia {

namespace detail {
struct http1_interim_response_prepare_result_access;
}

enum class http1_interim_response_prepare_error : std::uint8_t {
    invalid_header,
    too_many_headers,
    content_length_forbidden,
    transfer_encoding_forbidden,
    trailer_forbidden,
    te_field_forbidden,
    repeated_singleton,
    invalid_connection,
    invalid_upgrade,
    upgrade_connection_option_required,
    header_too_large,
};

// Connection lifecycle after the encoded interim response. RFC 9112 requires
// a sender of Connection: close to begin closing after the response containing
// that option, even though doing so leaves the request without a final response.
enum class http1_interim_connection_disposition : std::uint8_t {
    unchanged,
    close_after_interim_response,
};

[[nodiscard]] std::string_view http1_interim_response_prepare_error_message(
    http1_interim_response_prepare_error error) noexcept;

class http1_interim_response_buffer_too_small final {
public:
    [[nodiscard]] constexpr std::size_t required_head_bytes() const noexcept {
        return required_head_bytes_;
    }

private:
    friend struct detail::http1_interim_response_prepare_result_access;

    explicit constexpr http1_interim_response_buffer_too_small(std::size_t required_head_bytes) noexcept
        : required_head_bytes_(required_head_bytes) {}

    std::size_t required_head_bytes_;
};

// A transactionally encoded HTTP/1.1 interim head. The byte view points into
// the caller's output buffer. When Connection: close is present, the owner must
// initiate connection closure as soon as this interim head has been written.
class prepared_http1_interim_response final {
public:
    [[nodiscard]] constexpr std::string_view head() const noexcept {
        return head_;
    }

    [[nodiscard]] constexpr http1_interim_connection_disposition connection_disposition()
        const noexcept {
        return connection_disposition_;
    }

private:
    friend struct detail::http1_interim_response_prepare_result_access;

    constexpr prepared_http1_interim_response(
        std::string_view head, http1_interim_connection_disposition connection_disposition) noexcept
        : head_(head),
          connection_disposition_(connection_disposition) {}

    std::string_view head_;
    http1_interim_connection_disposition connection_disposition_;
};

class http1_interim_response_prepare_failure final {
public:
    [[nodiscard]] constexpr http1_interim_response_prepare_error error() const noexcept {
        return error_;
    }

private:
    friend struct detail::http1_interim_response_prepare_result_access;

    explicit constexpr http1_interim_response_prepare_failure(
        http1_interim_response_prepare_error error) noexcept
        : error_(error) {}

    http1_interim_response_prepare_error error_;
};

class http1_interim_response_prepare_result final {
public:
    [[nodiscard]] constexpr const http1_interim_response_buffer_too_small* buffer_too_small()
        const& noexcept {
        return std::get_if<http1_interim_response_buffer_too_small>(&state_);
    }
    const http1_interim_response_buffer_too_small* buffer_too_small() const&& = delete;

    [[nodiscard]] constexpr const prepared_http1_interim_response* prepared() const& noexcept {
        return std::get_if<prepared_http1_interim_response>(&state_);
    }
    const prepared_http1_interim_response* prepared() const&& = delete;

    [[nodiscard]] constexpr const http1_interim_response_prepare_failure* failure() const& noexcept {
        return std::get_if<http1_interim_response_prepare_failure>(&state_);
    }
    const http1_interim_response_prepare_failure* failure() const&& = delete;

private:
    friend struct detail::http1_interim_response_prepare_result_access;

    explicit constexpr http1_interim_response_prepare_result(
        http1_interim_response_buffer_too_small state_value) noexcept
        : state_(state_value) {}

    explicit constexpr http1_interim_response_prepare_result(
        prepared_http1_interim_response state_value) noexcept
        : state_(state_value) {}

    explicit constexpr http1_interim_response_prepare_result(
        http1_interim_response_prepare_failure state_value) noexcept
        : state_(state_value) {}

    std::variant<http1_interim_response_buffer_too_small, prepared_http1_interim_response,
        http1_interim_response_prepare_failure>
        state_;
};

// Allocation-free HTTP/1.1 interim-head writer. It validates the complete
// borrowed field set and exact 64-field/64-KiB bounds before touching the
// output buffer. No Server or Date field is injected: the encoded fields are
// exactly those represented by http_interim_response_head.
class http1_interim_response_writer final {
public:
    [[nodiscard]] http1_interim_response_prepare_result prepare(
        const http_interim_response_head& response, std::span<char> head_buffer) const noexcept;
};

}  // namespace ruvia
