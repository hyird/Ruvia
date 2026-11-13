#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <string_view>

#include "ruvia/http/http_response.h"

#include "response/http_response_header_bits.h"
#include "response/http_response_header_storage.h"

namespace ruvia::detail {

template <std::size_t n>
[[nodiscard]] consteval http_response_header static_response_header(
    std::string_view name, const char (&bytes_value)[n], std::uint32_t known_bit) {
    static_assert(n > 1 && response_header_storage_size_fits(0, n - 1));
    if (name.empty() || name.size() > n - 1 || bytes_value[n - 1] != '\0' ||
        !std::string_view(bytes_value, n - 1).starts_with(name)) {
        throw std::logic_error("invalid static response header storage");
    }
    if (known_bit != 0 && known_bit != response_header_upgrade &&
        response_known_header_slot(known_bit) == response_known_header_count) {
        throw std::logic_error("static response header requires one known header bit");
    }
    return http_response_header_access::make(
        bytes_value, static_cast<std::uint32_t>(name.size()),
        static_cast<std::uint32_t>(n - 1 - name.size()), known_bit, false);
}

template <std::size_t... sizes>
[[nodiscard]] consteval auto static_response_headers(
    std::string_view name, std::uint32_t known_bit, const char (&... bytes_values)[sizes]) {
    return std::array{static_response_header(name, bytes_values, known_bit)...};
}

[[nodiscard]] inline std::optional<http_response_header> find_static_response_header(
    std::span<const http_response_header> headers, std::string_view value) noexcept {
    for (const auto& header_value : headers) {
        if (header_value.value() == value) {
            return header_value;
        }
    }
    return std::nullopt;
}

inline constexpr auto content_type_headers = static_response_headers("Content-Type", response_header_content_type,
    "Content-Typetext/plain; charset=UTF-8",
    "Content-Typeapplication/json",
    "Content-Typetext/html; charset=UTF-8",
    "Content-Typetext/plain; charset=utf-8",
    "Content-Typeapplication/json; charset=utf-8",
    "Content-Typetext/html; charset=utf-8",
    "Content-Typetext/css; charset=utf-8",
    "Content-Typetext/javascript; charset=utf-8",
    "Content-Typetext/event-stream",
    "Content-Typeimage/png",
    "Content-Typeimage/jpeg",
    "Content-Typeimage/gif",
    "Content-Typeimage/svg+xml",
    "Content-Typeapplication/wasm",
    "Content-Typeapplication/octet-stream",
    "Content-Typeapplication/problem+json",
    "Content-Typeimage/apng",
    "Content-Typeimage/avif",
    "Content-Typeimage/webp",
    "Content-Typeimage/bmp",
    "Content-Typeimage/vnd.microsoft.icon",
    "Content-Typefont/woff",
    "Content-Typefont/woff2",
    "Content-Typefont/ttf",
    "Content-Typefont/otf",
    "Content-Typeapplication/vnd.ms-fontobject",
    "Content-Typetext/xml; charset=utf-8",
    "Content-Typeapplication/manifest+json; charset=utf-8");

[[nodiscard]] inline http_response_header static_content_type_header(http_content_type type) {
    switch (type) {
        case http_content_type::text_plain_utf8:
            return content_type_headers[0];
        case http_content_type::text_html_utf8:
            return content_type_headers[2];
        case http_content_type::application_json:
            return content_type_headers[1];
    }
    throw std::invalid_argument("invalid HTTP content type");
}

[[nodiscard]] inline std::optional<http_response_header> builtin_static_response_header(
    std::uint32_t known_bit, std::string_view value) noexcept {
    switch (known_bit) {
        case response_header_content_type:
            return find_static_response_header(content_type_headers, value);
        case response_header_connection: {
            static constexpr auto headers = static_response_headers("Connection", response_header_connection,
                "Connectionclose", "Connectionkeep-alive", "ConnectionUpgrade");
            return find_static_response_header(headers, value);
        }
        case response_header_accept_ranges: {
            static constexpr auto headers = static_response_headers("Accept-Ranges", response_header_accept_ranges,
                "Accept-Rangesbytes");
            return find_static_response_header(headers, value);
        }
        case response_header_content_encoding: {
            static constexpr auto headers = static_response_headers("Content-Encoding", response_header_content_encoding,
                "Content-Encodinggzip", "Content-Encodingbr", "Content-Encodingzstd", "Content-Encodingdeflate");
            return find_static_response_header(headers, value);
        }
        case response_header_transfer_encoding: {
            static constexpr auto headers = static_response_headers("Transfer-Encoding", response_header_transfer_encoding,
                "Transfer-Encodingchunked");
            return find_static_response_header(headers, value);
        }
        case response_header_cache_control: {
            static constexpr auto headers = static_response_headers("Cache-Control", response_header_cache_control,
                "Cache-Controlno-store", "Cache-Controlno-cache", "Cache-Controlprivate");
            return find_static_response_header(headers, value);
        }
        case response_header_vary: {
            static constexpr auto headers = static_response_headers("Vary", response_header_vary,
                "Vary*", "VaryAccept-Encoding", "VaryOrigin", "VaryAccess-Control-Request-Headers", "VaryAccess-Control-Request-Method");
            return find_static_response_header(headers, value);
        }
        case response_header_access_control_allow_origin: {
            static constexpr auto headers = static_response_headers("Access-Control-Allow-Origin", response_header_access_control_allow_origin,
                "Access-Control-Allow-Origin*");
            return find_static_response_header(headers, value);
        }
        case response_header_access_control_allow_credentials: {
            static constexpr auto headers = static_response_headers("Access-Control-Allow-Credentials", response_header_access_control_allow_credentials,
                "Access-Control-Allow-Credentialstrue");
            return find_static_response_header(headers, value);
        }
        case response_header_access_control_allow_methods: {
            static constexpr auto headers = static_response_headers("Access-Control-Allow-Methods", response_header_access_control_allow_methods,
                "Access-Control-Allow-MethodsGET, HEAD, POST, PUT, PATCH, DELETE, OPTIONS");
            return find_static_response_header(headers, value);
        }
        case response_header_upgrade: {
            static constexpr auto headers = static_response_headers("Upgrade", response_header_upgrade,
                "Upgradewebsocket", "Upgradeconnect-udp");
            return find_static_response_header(headers, value);
        }
        default:
            return std::nullopt;
    }
}

[[nodiscard]] inline std::optional<http_response_header> builtin_static_custom_response_header(
    std::string_view name, std::string_view value) noexcept {
    switch (name.size()) {
        case std::string_view("Retry-After").size():
            if (name == "Retry-After") {
                static constexpr auto headers = static_response_headers("Retry-After", 0, "Retry-After1");
                return find_static_response_header(headers, value);
            }
            return std::nullopt;
        case std::string_view("X-Frame-Options").size():
            if (name == "X-Frame-Options") {
                static constexpr auto headers = static_response_headers("X-Frame-Options", 0,
                    "X-Frame-OptionsDENY", "X-Frame-OptionsSAMEORIGIN");
                return find_static_response_header(headers, value);
            }
            if (name == "Referrer-Policy") {
                static constexpr auto headers = static_response_headers("Referrer-Policy", 0,
                    "Referrer-Policystrict-origin-when-cross-origin", "Referrer-Policyno-referrer");
                return find_static_response_header(headers, value);
            }
            if (name == "Accept-Encoding") {
                static constexpr auto headers = static_response_headers("Accept-Encoding", 0,
                    "Accept-Encodinggzip, deflate, br, zstd", "Accept-Encodingidentity");
                return find_static_response_header(headers, value);
            }
            return std::nullopt;
        case std::string_view("X-XSS-Protection").size():
            if (name == "X-XSS-Protection") {
                static constexpr auto headers = static_response_headers("X-XSS-Protection", 0, "X-XSS-Protection0");
                return find_static_response_header(headers, value);
            }
            if (name == "Capsule-Protocol") {
                static constexpr auto headers = static_response_headers("Capsule-Protocol", 0, "Capsule-Protocol?1");
                return find_static_response_header(headers, value);
            }
            return std::nullopt;
        case std::string_view("X-RateLimit-Reset").size():
            if (name == "X-RateLimit-Reset") {
                static constexpr auto headers = static_response_headers("X-RateLimit-Reset", 0, "X-RateLimit-Reset1");
                return find_static_response_header(headers, value);
            }
            return std::nullopt;
        case std::string_view("Permissions-Policy").size():
            if (name == "Permissions-Policy") {
                static constexpr auto headers = static_response_headers("Permissions-Policy", 0,
                    "Permissions-Policygeolocation=(), microphone=(), camera=()");
                return find_static_response_header(headers, value);
            }
            return std::nullopt;
        case std::string_view("X-RateLimit-Remaining").size():
            if (name == "X-RateLimit-Remaining") {
                static constexpr auto headers = static_response_headers("X-RateLimit-Remaining", 0, "X-RateLimit-Remaining0");
                return find_static_response_header(headers, value);
            }
            return std::nullopt;
        case std::string_view("X-Content-Type-Options").size():
            if (name == "X-Content-Type-Options") {
                static constexpr auto headers = static_response_headers("X-Content-Type-Options", 0, "X-Content-Type-Optionsnosniff");
                return find_static_response_header(headers, value);
            }
            return std::nullopt;
        case std::string_view("Content-Security-Policy").size():
            if (name == "Content-Security-Policy") {
                static constexpr auto headers = static_response_headers("Content-Security-Policy", 0,
                    "Content-Security-Policydefault-src 'self'", "Content-Security-Policydefault-src 'none'");
                return find_static_response_header(headers, value);
            }
            return std::nullopt;
        case std::string_view("Strict-Transport-Security").size():
            if (name == "Strict-Transport-Security") {
                static constexpr auto headers = static_response_headers("Strict-Transport-Security", 0,
                    "Strict-Transport-Securitymax-age=31536000; includeSubDomains");
                return find_static_response_header(headers, value);
            }
            return std::nullopt;
        default:
            return std::nullopt;
    }
}

}  // namespace ruvia::detail
