#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <type_traits>

#include "ruvia/http/http1_chunk_decode_error.h"
#include "ruvia/http/http_protocol_error.h"
#include "ruvia/http/http_transfer_coding_decode_error.h"
#include "ruvia/http/protocol_byte_limit.h"

namespace ruvia {

// A request-body failure detected by a server driver while enforcing the
// HTTP-owned content contract. The driver supplies runtime byte limits and I/O
// completion facts; HTTP owns their final response status and diagnostic.
class http_request_body_failure final {
public:
    [[nodiscard]] static constexpr http_request_body_failure too_large() noexcept {
        return http_request_body_failure(kind_type::too_large);
    }

    [[nodiscard]] static constexpr http_request_body_failure incomplete() noexcept {
        return http_request_body_failure(kind_type::incomplete);
    }

    [[nodiscard]] http_protocol_error protocol_error() const noexcept {
        switch (kind_) {
            case kind_type::too_large:
                return http_protocol_error(
                    http_status::content_too_large, "request body is too large");
            case kind_type::incomplete:
                return http_protocol_error(http_status::bad_request, "incomplete request body");
        }
        return http_protocol_error(http_status::bad_request, "invalid request body");
    }

private:
    enum class kind_type : std::uint8_t { too_large,
        incomplete };

    explicit constexpr http_request_body_failure(kind_type kind) noexcept
        : kind_(kind) {}

    kind_type kind_;
};

[[nodiscard]] inline http_protocol_error http_request_chunk_decode_error(
    http1_chunk_decode_error error) noexcept {
    switch (error) {
        case http1_chunk_decode_error::invalid_framing:
            return http_protocol_error(http_status::bad_request, "invalid chunked request body");
        case http1_chunk_decode_error::body_limit_exceeded:
            return http_request_body_failure::too_large().protocol_error();
        case http1_chunk_decode_error::framing_limit_exceeded:
            return http_protocol_error(
                http_status::content_too_large, "request body framing is too large");
    }
    return http_protocol_error(http_status::bad_request, "invalid chunked request body");
}

[[nodiscard]] inline http_protocol_error http_request_transfer_coding_error(
    http_transfer_coding_decode_error error) noexcept {
    switch (error) {
        case http_transfer_coding_decode_error::invalid_content:
            return http_protocol_error(http_status::bad_request, "invalid transfer-coding body");
        case http_transfer_coding_decode_error::decoded_size_exceeded:
            return http_request_body_failure::too_large().protocol_error();
    }
    return http_protocol_error(http_status::bad_request, "invalid transfer-coding body");
}

[[nodiscard]] inline std::optional<http_request_body_failure> http_request_body_size_failure(
    std::size_t size, protocol_byte_limit limit) noexcept {
    return limit.exceeds(size)
               ? std::optional<http_request_body_failure>(http_request_body_failure::too_large())
               : std::nullopt;
}

[[nodiscard]] inline std::optional<http_request_body_failure> http_request_body_addition_failure(
    std::size_t current_size, std::size_t additional_size, protocol_byte_limit limit) noexcept {
    return limit.addition_exceeds(current_size, additional_size)
               ? std::optional<http_request_body_failure>(http_request_body_failure::too_large())
               : std::nullopt;
}

static_assert(std::is_trivially_copyable_v<http_request_body_failure>);
static_assert(sizeof(http_request_body_failure) <= 1);

}  // namespace ruvia
