#pragma once

#include <cstdint>
#include <optional>
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
    if (response_known_header_slot(known_bit) == response_known_header_count) {
        throw "static response header requires one known header bit";
    }
    return make_response_header(
        bytes_value, name_size, static_cast<std::uint32_t>(n - 1 - name_size), known_bit, false);
}

[[nodiscard]] inline std::optional<http_response_header> builtin_static_response_header(
    std::uint32_t known_bit, std::string_view value) noexcept {
    static constexpr char text_content_type[] = "Content-Typetext/plain; charset=UTF-8";
    static constexpr char json_content_type[] = "Content-Typeapplication/json";
    static constexpr char html_content_type[] = "Content-Typetext/html; charset=UTF-8";
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
    static constexpr char transfer_encoding_chunked[] = "Transfer-Encodingchunked";
    static constexpr char cache_control_no_store[] = "Cache-Controlno-store";
    static constexpr char vary_accept_encoding[] = "VaryAccept-Encoding";
    static constexpr char vary_origin[] = "VaryOrigin";
    static constexpr char vary_access_control_request_headers[] = "VaryAccess-Control-Request-Headers";
    static constexpr char vary_access_control_request_method[] = "VaryAccess-Control-Request-Method";
    static constexpr char access_control_allow_credentials_true[] =
        "Access-Control-Allow-Credentialstrue";

    switch (known_bit) {
        case response_header_content_type:
            if (value == "text/plain; charset=UTF-8") {
                return static_response_header(text_content_type, 12, response_header_content_type);
            }
            if (value == "application/json") {
                return static_response_header(json_content_type, 12, response_header_content_type);
            }
            if (value == "text/html; charset=UTF-8") {
                return static_response_header(html_content_type, 12, response_header_content_type);
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
            return std::nullopt;
        case response_header_vary:
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
        case response_header_access_control_allow_credentials:
            if (value == "true") {
                return static_response_header(access_control_allow_credentials_true, 32, response_header_access_control_allow_credentials);
            }
            return std::nullopt;
        default:
            return std::nullopt;
    }
}

}  // namespace ruvia::detail
