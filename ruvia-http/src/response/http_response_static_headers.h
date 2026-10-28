#pragma once

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string_view>

#include "ruvia/http/detail/response/http_response_header_bits.h"
#include "ruvia/http/http_response.h"

#include "response/http_response_header_access.h"

namespace ruvia::detail {

template <std::size_t n>
[[nodiscard]] consteval http_response_header static_response_header(
    const char (&bytes_value)[n], std::uint32_t name_size, std::uint32_t known_bit) {
    static_assert(n > 1 && response_header_storage_size_fits(0, n - 1));
    if (name_size == 0 || name_size > n - 1 || bytes_value[n - 1] != '\0') {
        throw "invalid static response header storage";
    }
    if (known_bit != 0 && response_known_header_slot(known_bit) == response_known_header_count) {
        throw "static response header requires one known header bit";
    }
    return http_response_header_access::make(
        bytes_value, name_size, static_cast<std::uint32_t>(n - 1 - name_size), known_bit, false);
}

inline constexpr char text_plain_utf8_content_type[] = "Content-Typetext/plain; charset=UTF-8";
inline constexpr char application_json_content_type[] = "Content-Typeapplication/json";
inline constexpr char text_html_utf8_content_type[] = "Content-Typetext/html; charset=UTF-8";

[[nodiscard]] inline http_response_header static_content_type_header(http_content_type type) {
    switch (type) {
        case http_content_type::text_plain_utf8:
            return static_response_header(text_plain_utf8_content_type, 12, response_header_content_type);
        case http_content_type::text_html_utf8:
            return static_response_header(text_html_utf8_content_type, 12, response_header_content_type);
        case http_content_type::application_json:
            return static_response_header(application_json_content_type, 12, response_header_content_type);
    }
    throw std::invalid_argument("invalid HTTP content type");
}

[[nodiscard]] inline std::optional<http_response_header> builtin_static_response_header(
    std::uint32_t known_bit, std::string_view value) noexcept {
    static constexpr char lowercase_utf8_text_content_type[] = "Content-Typetext/plain; charset=utf-8";
    static constexpr char lowercase_utf8_json_content_type[] =
        "Content-Typeapplication/json; charset=utf-8";
    static constexpr char lowercase_utf8_html_content_type[] = "Content-Typetext/html; charset=utf-8";
    static constexpr char css_content_type[] = "Content-Typetext/css; charset=utf-8";
    static constexpr char js_content_type[] = "Content-Typetext/javascript; charset=utf-8";
    static constexpr char event_stream_content_type[] = "Content-Typetext/event-stream";
    static constexpr char png_content_type[] = "Content-Typeimage/png";
    static constexpr char jpeg_content_type[] = "Content-Typeimage/jpeg";
    static constexpr char gif_content_type[] = "Content-Typeimage/gif";
    static constexpr char svg_content_type[] = "Content-Typeimage/svg+xml";
    static constexpr char wasm_content_type[] = "Content-Typeapplication/wasm";
    static constexpr char octet_stream_content_type[] = "Content-Typeapplication/octet-stream";
    static constexpr char connection_close[] = "Connectionclose";
    static constexpr char accept_ranges_bytes[] = "Accept-Rangesbytes";
    static constexpr char content_encoding_gzip[] = "Content-Encodinggzip";
    static constexpr char content_encoding_br[] = "Content-Encodingbr";
    static constexpr char content_encoding_zstd[] = "Content-Encodingzstd";
    static constexpr char content_encoding_deflate[] = "Content-Encodingdeflate";
    static constexpr char transfer_encoding_chunked[] = "Transfer-Encodingchunked";
    static constexpr char cache_control_no_store[] = "Cache-Controlno-store";
    static constexpr char cache_control_no_cache[] = "Cache-Controlno-cache";
    static constexpr char cache_control_private[] = "Cache-Controlprivate";
    static constexpr char vary_wildcard[] = "Vary*";
    static constexpr char vary_accept_encoding[] = "VaryAccept-Encoding";
    static constexpr char vary_origin[] = "VaryOrigin";
    static constexpr char vary_access_control_request_headers[] = "VaryAccess-Control-Request-Headers";
    static constexpr char vary_access_control_request_method[] = "VaryAccess-Control-Request-Method";
    static constexpr char access_control_allow_origin_wildcard[] =
        "Access-Control-Allow-Origin*";
    static constexpr char access_control_allow_credentials_true[] =
        "Access-Control-Allow-Credentialstrue";

    switch (known_bit) {
        case response_header_content_type:
            if (value == "text/plain; charset=UTF-8") {
                return static_content_type_header(http_content_type::text_plain_utf8);
            }
            if (value == "application/json") {
                return static_content_type_header(http_content_type::application_json);
            }
            if (value == "text/html; charset=UTF-8") {
                return static_content_type_header(http_content_type::text_html_utf8);
            }
            if (value == "text/plain; charset=utf-8") {
                return static_response_header(lowercase_utf8_text_content_type, 12, response_header_content_type);
            }
            if (value == "application/json; charset=utf-8") {
                return static_response_header(lowercase_utf8_json_content_type, 12, response_header_content_type);
            }
            if (value == "text/html; charset=utf-8") {
                return static_response_header(lowercase_utf8_html_content_type, 12, response_header_content_type);
            }
            if (value == "text/css; charset=utf-8") {
                return static_response_header(css_content_type, 12, response_header_content_type);
            }
            if (value == "text/javascript; charset=utf-8") {
                return static_response_header(js_content_type, 12, response_header_content_type);
            }
            if (value == "text/event-stream") {
                return static_response_header(event_stream_content_type, 12, response_header_content_type);
            }
            if (value == "image/png") {
                return static_response_header(png_content_type, 12, response_header_content_type);
            }
            if (value == "image/jpeg") {
                return static_response_header(jpeg_content_type, 12, response_header_content_type);
            }
            if (value == "image/gif") {
                return static_response_header(gif_content_type, 12, response_header_content_type);
            }
            if (value == "image/svg+xml") {
                return static_response_header(svg_content_type, 12, response_header_content_type);
            }
            if (value == "application/wasm") {
                return static_response_header(wasm_content_type, 12, response_header_content_type);
            }
            if (value == "application/octet-stream") {
                return static_response_header(octet_stream_content_type, 12, response_header_content_type);
            }
            return std::nullopt;
        case response_header_connection:
            if (value == "close") {
                return static_response_header(connection_close, 10, response_header_connection);
            }
            return std::nullopt;
        case response_header_accept_ranges:
            if (value == "bytes") {
                return static_response_header(accept_ranges_bytes, 13, response_header_accept_ranges);
            }
            return std::nullopt;
        case response_header_content_encoding:
            if (value == "gzip") {
                return static_response_header(content_encoding_gzip, 16, response_header_content_encoding);
            }
            if (value == "br") {
                return static_response_header(content_encoding_br, 16, response_header_content_encoding);
            }
            if (value == "zstd") {
                return static_response_header(content_encoding_zstd, 16, response_header_content_encoding);
            }
            if (value == "deflate") {
                return static_response_header(content_encoding_deflate, 16, response_header_content_encoding);
            }
            return std::nullopt;
        case response_header_transfer_encoding:
            if (value == "chunked") {
                return static_response_header(transfer_encoding_chunked, 17, response_header_transfer_encoding);
            }
            return std::nullopt;
        case response_header_cache_control:
            if (value == "no-store") {
                return static_response_header(cache_control_no_store, 13, response_header_cache_control);
            }
            if (value == "no-cache") {
                return static_response_header(cache_control_no_cache, 13, response_header_cache_control);
            }
            if (value == "private") {
                return static_response_header(cache_control_private, 13, response_header_cache_control);
            }
            return std::nullopt;
        case response_header_vary:
            if (value == "*") {
                return static_response_header(vary_wildcard, 4, response_header_vary);
            }
            if (value == "Accept-Encoding") {
                return static_response_header(vary_accept_encoding, 4, response_header_vary);
            }
            if (value == "Origin") {
                return static_response_header(vary_origin, 4, response_header_vary);
            }
            if (value == "Access-Control-Request-Headers") {
                return static_response_header(vary_access_control_request_headers, 4, response_header_vary);
            }
            if (value == "Access-Control-Request-Method") {
                return static_response_header(vary_access_control_request_method, 4, response_header_vary);
            }
            return std::nullopt;
        case response_header_access_control_allow_origin:
            if (value == "*") {
                return static_response_header(access_control_allow_origin_wildcard, 27, response_header_access_control_allow_origin);
            }
            return std::nullopt;
        case response_header_access_control_allow_credentials:
            if (value == "true") {
                return static_response_header(access_control_allow_credentials_true, 32, response_header_access_control_allow_credentials);
            }
            return std::nullopt;
        default:
            return std::nullopt;
    }
}

[[nodiscard]] inline std::optional<http_response_header> builtin_static_custom_response_header(
    std::string_view name, std::string_view value) noexcept {
    static constexpr char x_content_type_options_nosniff[] = "X-Content-Type-Optionsnosniff";
    static constexpr char x_frame_options_deny[] = "X-Frame-OptionsDENY";
    static constexpr char x_frame_options_sameorigin[] = "X-Frame-OptionsSAMEORIGIN";
    static constexpr char strict_transport_security_default[] =
        "Strict-Transport-Securitymax-age=31536000; includeSubDomains";
    static constexpr char x_xss_protection_zero[] = "X-XSS-Protection0";
    static constexpr char content_security_policy_self[] =
        "Content-Security-Policydefault-src 'self'";
    static constexpr char content_security_policy_none[] =
        "Content-Security-Policydefault-src 'none'";
    static constexpr char referrer_policy_strict_origin[] =
        "Referrer-Policystrict-origin-when-cross-origin";
    static constexpr char referrer_policy_no_referrer[] = "Referrer-Policyno-referrer";
    static constexpr char permissions_policy_default[] =
        "Permissions-Policygeolocation=(), microphone=(), camera=()";
    static constexpr char access_control_allow_methods_default[] =
        "Access-Control-Allow-MethodsGET, HEAD, POST, PUT, PATCH, DELETE, OPTIONS";

    switch (name.size()) {
        case 15:
            if (name == "X-Frame-Options") {
                if (value == "DENY") {
                    return static_response_header(x_frame_options_deny, 15, 0);
                }
                if (value == "SAMEORIGIN") {
                    return static_response_header(x_frame_options_sameorigin, 15, 0);
                }
                return std::nullopt;
            }
            if (name == "Referrer-Policy") {
                if (value == "strict-origin-when-cross-origin") {
                    return static_response_header(referrer_policy_strict_origin, 15, 0);
                }
                if (value == "no-referrer") {
                    return static_response_header(referrer_policy_no_referrer, 15, 0);
                }
            }
            return std::nullopt;
        case 16:
            if (name == "X-XSS-Protection" && value == "0") {
                return static_response_header(x_xss_protection_zero, 16, 0);
            }
            return std::nullopt;
        case 18:
            if (name == "Permissions-Policy" &&
                value == "geolocation=(), microphone=(), camera=()") {
                return static_response_header(permissions_policy_default, 18, 0);
            }
            return std::nullopt;
        case 22:
            if (name == "X-Content-Type-Options" && value == "nosniff") {
                return static_response_header(x_content_type_options_nosniff, 22, 0);
            }
            return std::nullopt;
        case 23:
            if (name == "Content-Security-Policy") {
                if (value == "default-src 'self'") {
                    return static_response_header(content_security_policy_self, 23, 0);
                }
                if (value == "default-src 'none'") {
                    return static_response_header(content_security_policy_none, 23, 0);
                }
            }
            return std::nullopt;
        case 25:
            if (name == "Strict-Transport-Security" &&
                value == "max-age=31536000; includeSubDomains") {
                return static_response_header(strict_transport_security_default, 25, 0);
            }
            return std::nullopt;
        case 28:
            if (name == "Access-Control-Allow-Methods" &&
                value == "GET, HEAD, POST, PUT, PATCH, DELETE, OPTIONS") {
                return static_response_header(access_control_allow_methods_default, 28, 0);
            }
            return std::nullopt;
        default:
            return std::nullopt;
    }
}

}  // namespace ruvia::detail
