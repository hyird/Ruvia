#pragma once

#include <cstdint>
#include <string_view>

namespace ruvia::detail {

struct HttpStatusEntry final {
    std::uint16_t code;
    std::string_view text;
    std::string_view line;
};

[[nodiscard]] inline const HttpStatusEntry* httpStatusEntry(std::uint16_t statusCode) noexcept {
    switch (statusCode) {
        case 200: { static constexpr HttpStatusEntry entry{200, "OK", "HTTP/1.1 200 OK\r\n"}; return &entry; }
        case 201: { static constexpr HttpStatusEntry entry{201, "Created", "HTTP/1.1 201 Created\r\n"}; return &entry; }
        case 202: { static constexpr HttpStatusEntry entry{202, "Accepted", "HTTP/1.1 202 Accepted\r\n"}; return &entry; }
        case 204: { static constexpr HttpStatusEntry entry{204, "No Content", "HTTP/1.1 204 No Content\r\n"}; return &entry; }
        case 206: { static constexpr HttpStatusEntry entry{206, "Partial Content", "HTTP/1.1 206 Partial Content\r\n"}; return &entry; }
        case 301: { static constexpr HttpStatusEntry entry{301, "Moved Permanently", "HTTP/1.1 301 Moved Permanently\r\n"}; return &entry; }
        case 302: { static constexpr HttpStatusEntry entry{302, "Found", "HTTP/1.1 302 Found\r\n"}; return &entry; }
        case 303: { static constexpr HttpStatusEntry entry{303, "See Other", "HTTP/1.1 303 See Other\r\n"}; return &entry; }
        case 304: { static constexpr HttpStatusEntry entry{304, "Not Modified", "HTTP/1.1 304 Not Modified\r\n"}; return &entry; }
        case 307: { static constexpr HttpStatusEntry entry{307, "Temporary Redirect", "HTTP/1.1 307 Temporary Redirect\r\n"}; return &entry; }
        case 308: { static constexpr HttpStatusEntry entry{308, "Permanent Redirect", "HTTP/1.1 308 Permanent Redirect\r\n"}; return &entry; }
        case 400: { static constexpr HttpStatusEntry entry{400, "Bad Request", "HTTP/1.1 400 Bad Request\r\n"}; return &entry; }
        case 401: { static constexpr HttpStatusEntry entry{401, "Unauthorized", "HTTP/1.1 401 Unauthorized\r\n"}; return &entry; }
        case 403: { static constexpr HttpStatusEntry entry{403, "Forbidden", "HTTP/1.1 403 Forbidden\r\n"}; return &entry; }
        case 404: { static constexpr HttpStatusEntry entry{404, "Not Found", "HTTP/1.1 404 Not Found\r\n"}; return &entry; }
        case 405: { static constexpr HttpStatusEntry entry{405, "Method Not Allowed", "HTTP/1.1 405 Method Not Allowed\r\n"}; return &entry; }
        case 409: { static constexpr HttpStatusEntry entry{409, "Conflict", "HTTP/1.1 409 Conflict\r\n"}; return &entry; }
        case 412: { static constexpr HttpStatusEntry entry{412, "Precondition Failed", "HTTP/1.1 412 Precondition Failed\r\n"}; return &entry; }
        case 413: { static constexpr HttpStatusEntry entry{413, "Payload Too Large", "HTTP/1.1 413 Payload Too Large\r\n"}; return &entry; }
        case 416: { static constexpr HttpStatusEntry entry{416, "Range Not Satisfiable", "HTTP/1.1 416 Range Not Satisfiable\r\n"}; return &entry; }
        case 417: { static constexpr HttpStatusEntry entry{417, "Expectation Failed", "HTTP/1.1 417 Expectation Failed\r\n"}; return &entry; }
        case 418: { static constexpr HttpStatusEntry entry{418, "I'm a Teapot", "HTTP/1.1 418 I'm a Teapot\r\n"}; return &entry; }
        case 422: { static constexpr HttpStatusEntry entry{422, "Unprocessable Entity", "HTTP/1.1 422 Unprocessable Entity\r\n"}; return &entry; }
        case 429: { static constexpr HttpStatusEntry entry{429, "Too Many Requests", "HTTP/1.1 429 Too Many Requests\r\n"}; return &entry; }
        case 431: { static constexpr HttpStatusEntry entry{431, "Request Header Fields Too Large", "HTTP/1.1 431 Request Header Fields Too Large\r\n"}; return &entry; }
        case 500: { static constexpr HttpStatusEntry entry{500, "Internal Server Error", "HTTP/1.1 500 Internal Server Error\r\n"}; return &entry; }
        case 501: { static constexpr HttpStatusEntry entry{501, "Not Implemented", "HTTP/1.1 501 Not Implemented\r\n"}; return &entry; }
        case 505: { static constexpr HttpStatusEntry entry{505, "HTTP Version Not Supported", "HTTP/1.1 505 HTTP Version Not Supported\r\n"}; return &entry; }
        default:
            return nullptr;
    }
}

[[nodiscard]] inline std::string_view httpDefaultStatusText(std::uint16_t statusCode) noexcept {
    if (const auto* entry = httpStatusEntry(statusCode); entry != nullptr) {
        return entry->text;
    }
    return statusCode >= 500 ? "Internal Server Error" : "Bad Request";
}

[[nodiscard]] inline std::string_view httpCachedStatusLine(
    std::uint16_t statusCode,
    std::string_view statusText) noexcept {
    const auto* entry = httpStatusEntry(statusCode);
    if (entry == nullptr || entry->text != statusText) {
        return {};
    }
    return entry->line;
}

}  // namespace ruvia::detail
