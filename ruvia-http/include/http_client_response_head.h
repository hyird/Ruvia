#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "borrowed_text.h"
#include "http_header.h"
#include "http_message.h"
#include "http_request.h"
#include "http_status.h"

namespace ruvia::detail {
struct http_client_response_head_access;
}  // namespace ruvia::detail
namespace ruvia {
// A response-side signal for an external request-body writer. The same signal
// is emitted by HTTP/1 response plans and HTTP/2 response events.
enum class http_client_request_content_signal : std::uint8_t {
    continue_value,
    exchange_complete,
};

// Owns response fields.
// Decoding a head without fields does not allocate field storage.
// HEAD response fields remain metadata even for status 205: Content-Length
// does not become the response's received-content length.
class http_client_response_head final {
public:
    http_client_response_head(const http_client_response_head&) = delete;
    http_client_response_head& operator=(const http_client_response_head&) = delete;
    // Moving transfers the owned field storage. The source retains its
    // status and version and exposes empty fields.
    http_client_response_head(http_client_response_head&&) noexcept = default;
    http_client_response_head& operator=(http_client_response_head&&) = delete;

    // The status the response is processed as: a received 600..999 is
    // processed as 500 (RFC 9110 section 15).
    [[nodiscard]] http_status_code status() const noexcept {
        return status_.status();
    }

    // The status code as received, including an invalid 600..999.
    [[nodiscard]] http_received_status received_status() const noexcept {
        return status_;
    }

    [[nodiscard]] http_protocol_version protocol_version() const noexcept {
        return protocol_version_;
    }
    [[nodiscard]] std::optional<std::uint64_t> content_length() const noexcept {
        return content_length_;
    }

    // Parsed field lines retain wire order and repeated metadata fields. In
    // particular, Content-Type values are not parsed as media types, merged,
    // or selected for the application, even when their media-type syntax is invalid.
    // Content-Encoding is also retained without interpreting coding-list syntax;
    // content-decoding APIs validate it independently of message framing.
    [[nodiscard]] std::span<const http_header> headers() const& noexcept RUVIA_LIFETIMEBOUND {
        return headers_;
    }
    [[nodiscard]] std::span<const http_header> headers() const&& = delete;

    // Case-insensitive lookup; the last repeated field wins.
    [[nodiscard]] std::optional<std::string_view> header(std::string_view name) const& noexcept RUVIA_LIFETIMEBOUND;
    std::optional<std::string_view> header(std::string_view name) const&& = delete;

    // Transfers the owned parsed fields to a consumer that retains the head's
    // metadata but needs to extend the fields' lifetime independently.
    [[nodiscard]] std::vector<http_header> take_headers() && noexcept {
        return std::move(headers_);
    }

private:
    friend struct detail::http_client_response_head_access;

    http_client_response_head(http_received_status status, http_protocol_version protocol_version) noexcept :
        status_(status),
        protocol_version_(protocol_version) {}

    http_received_status status_;
    http_protocol_version protocol_version_;
    std::vector<http_header> headers_;
    std::optional<std::uint64_t> content_length_;
};

inline std::optional<std::string_view> http_client_response_head::header(std::string_view name) const& noexcept {
    return detail::find_last_http_header<http_header>(headers_, name);
}

}  // namespace ruvia
