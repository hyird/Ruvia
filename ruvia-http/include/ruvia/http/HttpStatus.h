#pragma once

#include <cstdint>
#include <string_view>

namespace ruvia {

// RFC 9112 reason-phrase is optional HTTP/1 presentation text, not response
// semantics. Known codes get a conventional phrase; extension/unregistered
// codes deliberately get an empty phrase instead of being mislabeled by class.
[[nodiscard]] inline constexpr std::string_view httpReasonPhrase(
    std::uint16_t statusCode) noexcept {
    switch (statusCode) {
        case 100: return "Continue";
        case 101: return "Switching Protocols";
        case 102: return "Processing";
        case 103: return "Early Hints";
        case 200: return "OK";
        case 201: return "Created";
        case 202: return "Accepted";
        case 203: return "Non-Authoritative Information";
        case 204: return "No Content";
        case 205: return "Reset Content";
        case 206: return "Partial Content";
        case 301: return "Moved Permanently";
        case 302: return "Found";
        case 303: return "See Other";
        case 304: return "Not Modified";
        case 307: return "Temporary Redirect";
        case 308: return "Permanent Redirect";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 409: return "Conflict";
        case 412: return "Precondition Failed";
        case 413: return "Payload Too Large";
        case 416: return "Range Not Satisfiable";
        case 417: return "Expectation Failed";
        case 418: return "I'm a Teapot";
        case 422: return "Unprocessable Entity";
        case 426: return "Upgrade Required";
        case 429: return "Too Many Requests";
        case 431: return "Request Header Fields Too Large";
        case 500: return "Internal Server Error";
        case 501: return "Not Implemented";
        case 502: return "Bad Gateway";
        case 503: return "Service Unavailable";
        case 504: return "Gateway Timeout";
        case 505: return "HTTP Version Not Supported";
        default: return {};
    }
}

namespace detail {

// RFC 9110 section 15 reserves the complete HTTP status-code space to the
// five defined classes. Values in 600..999 are commonly used as library-local
// sentinels, but they are not valid status codes and must never reach a wire
// response. Keep the final-response subset named as well: an informational
// response is a distinct message and cannot terminate a request exchange.
[[nodiscard]] inline constexpr bool httpStatusCodeValid(
    std::uint16_t statusCode) noexcept {
    return statusCode >= 100 && statusCode <= 599;
}

[[nodiscard]] inline constexpr bool httpFinalStatusCodeValid(
    std::uint16_t statusCode) noexcept {
    return statusCode >= 200 && statusCode <= 599;
}

// 101 is a protocol transition rather than an interim progress head. It is
// intentionally owned by a dedicated Upgrade driver instead of either generic
// response-head type.
[[nodiscard]] inline constexpr bool httpInterimStatusCodeValid(
    std::uint16_t statusCode) noexcept {
    return statusCode >= 100 && statusCode < 200 && statusCode != 101;
}

}  // namespace detail

}  // namespace ruvia
