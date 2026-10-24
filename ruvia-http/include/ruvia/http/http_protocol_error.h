#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <exception>
#include <string_view>

#include "ruvia/http/http_status.h"

namespace ruvia {

// An HTTP protocol primitive rejected input with a response status that a
// server driver may map onto its own application error model. `code` is a
// stable snake_case machine token naming the specific failure (for example
// "missing_host") and must have static storage duration: it is kept as a view.
// The human-readable message is copied into fixed exception storage, so
// reporting never allocates or borrows.
class http_protocol_error final : public std::exception {
public:
    http_protocol_error(
        http_status_code status, std::string_view code, std::string_view message) noexcept
        : status_(status),
          code_(code) {
        const auto size = (std::min)(message.size(), message_.size() - 1);
        if (size != 0) {
            std::memcpy(message_.data(), message.data(), size);
        }
        message_[size] = '\0';
    }

    [[nodiscard]] http_status_code status() const noexcept {
        return status_;
    }

    [[nodiscard]] std::string_view code() const noexcept {
        return code_;
    }

    [[nodiscard]] const char* what() const noexcept override {
        return message_.data();
    }

private:
    http_status_code status_;
    std::string_view code_;
    std::array<char, 128> message_{};
};

enum class http_parse_error : std::uint8_t {
    header_too_large,
    uri_too_long,
    invalid_request_line,
    unsupported_http_version,
    invalid_request_target,
    invalid_header,
    invalid_connection,
    invalid_upgrade,
    missing_host,
    invalid_host,
    invalid_content_length,
    conflicting_content_length,
    invalid_transfer_encoding,
    unsupported_transfer_encoding
};

[[nodiscard]] http_protocol_error http_parse_protocol_error(http_parse_error error) noexcept;

}  // namespace ruvia
