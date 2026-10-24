#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <span>
#include <string_view>
#include <variant>
#include <vector>

#include "ruvia/http/http_header.h"
#include "ruvia/http/http_status.h"

namespace ruvia {

// Immutable, bodyless response-head view for 1xx progress messages that precede
// a final response. 101 is deliberately excluded: switching protocols transfers
// connection ownership and therefore requires its dedicated protocol driver.
// Header elements and their strings are borrowed and must remain stable through
// the synchronous sans-I/O submit call.
class http_interim_response_head final {
public:
    class header_init_type final {
    public:
        constexpr header_init_type() noexcept
            : headers_() {}

        constexpr header_init_type(std::span<const http_header_view> headers) noexcept
            : headers_(headers) {}

        template <std::size_t n>
        constexpr header_init_type(const http_header_view (&headers)[n]) noexcept
            : headers_(headers, n) {}

        template <std::size_t n>
        constexpr header_init_type(const std::array<http_header_view, n>& headers) noexcept
            : headers_(headers) {}

        template <std::size_t n>
        header_init_type(std::array<http_header_view, n>&&) = delete;

        template <typename allocator_type>
        header_init_type(const std::vector<http_header_view, allocator_type>&) = delete;

        constexpr header_init_type(std::initializer_list<http_header_view>) = delete;

        [[nodiscard]] constexpr operator std::span<const http_header_view>() const noexcept {
            return headers_;
        }

    private:
        std::span<const http_header_view> headers_;
    };

    explicit http_interim_response_head(http_status_code status_code, header_init_type headers = {});

    [[nodiscard]] constexpr http_status_code status() const noexcept {
        return status_code_;
    }

    [[nodiscard]] constexpr std::span<const http_header_view> headers() const noexcept {
        return headers_;
    }

private:
    http_status_code status_code_;
    std::span<const http_header_view> headers_;
};

enum class http1_interim_response_prepare_error : std::uint8_t {
    invalid_header,
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
    friend std::variant<class prepared_http1_interim_response, http1_interim_response_buffer_too_small,
        http1_interim_response_prepare_error>
    prepare_http1_interim_response_head(const http_interim_response_head&, std::span<char>) noexcept;

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
    friend std::variant<prepared_http1_interim_response, http1_interim_response_buffer_too_small,
        http1_interim_response_prepare_error>
    prepare_http1_interim_response_head(const http_interim_response_head&, std::span<char>) noexcept;

    constexpr prepared_http1_interim_response(
        std::string_view head, http1_interim_connection_disposition connection_disposition) noexcept
        : head_(head),
          connection_disposition_(connection_disposition) {}

    std::string_view head_;
    http1_interim_connection_disposition connection_disposition_;
};

// Allocation-free HTTP/1.1 interim-head writer. It validates the complete
// borrowed field set and exact 64-field/64-KiB bounds before touching the
// output buffer. No Server or Date field is injected: the encoded fields are
// exactly those represented by http_interim_response_head.
[[nodiscard]] std::variant<prepared_http1_interim_response, http1_interim_response_buffer_too_small,
    http1_interim_response_prepare_error>
prepare_http1_interim_response_head(
    const http_interim_response_head& response, std::span<char> head_buffer) noexcept;

}  // namespace ruvia
