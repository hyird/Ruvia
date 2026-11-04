#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <variant>

#include "ruvia/http/http1_response_head_error.h"
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
    explicit http_interim_response_head(http_status_code status_code, borrowed_http_headers headers = {});

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

// Connection lifecycle after the encoded interim response. RFC 9112 requires
// a sender of Connection: close to begin closing after the response containing
// that option, even though doing so leaves the request without a final response.
enum class http1_interim_connection_disposition : std::uint8_t {
    unchanged,
    close_after_interim_response,
};

class http1_interim_response_buffer_too_small final {
public:
    [[nodiscard]] constexpr std::size_t required_head_bytes() const noexcept {
        return required_head_bytes_;
    }

private:
    friend std::variant<class prepared_http1_interim_response, http1_interim_response_buffer_too_small>
    prepare_http1_interim_response_head(const http_interim_response_head&, std::span<char>);

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
    friend std::variant<prepared_http1_interim_response, http1_interim_response_buffer_too_small>
    prepare_http1_interim_response_head(const http_interim_response_head&, std::span<char>);

    constexpr prepared_http1_interim_response(
        std::string_view head, http1_interim_connection_disposition connection_disposition) noexcept
        : head_(head),
          connection_disposition_(connection_disposition) {}

    std::string_view head_;
    http1_interim_connection_disposition connection_disposition_;
};

// Allocation-free HTTP/1.1 interim-head writer. It validates the complete
// borrowed field set and exact 64-field/64-KiB bounds before touching the
// output buffer, throwing http1_response_head_error for a head it cannot send.
// No Server or Date field is injected: the encoded fields are exactly those
// represented by http_interim_response_head.
[[nodiscard]] std::variant<prepared_http1_interim_response, http1_interim_response_buffer_too_small>
prepare_http1_interim_response_head(const http_interim_response_head& response, std::span<char> head_buffer);

}  // namespace ruvia
