#include "ruvia/http/HttpResponse.h"

#include "ruvia/http/HeaderUtils.h"

namespace ruvia {
namespace {

[[nodiscard]] unsigned char lowerAscii(unsigned char c) noexcept {
    return c >= 'A' && c <= 'Z' ? static_cast<unsigned char>(c + ('a' - 'A')) : c;
}

}  // namespace

std::uint32_t HttpResponse::classifyKnownHeader(std::string_view name) noexcept {
    if (name.empty()) {
        return 0;
    }
    const auto first = lowerAscii(static_cast<unsigned char>(name.front()));
    switch (name.size()) {
        case 4:
            switch (first) {
                case 'd':
                    if (detail::httpAsciiEqualsIgnoreCase(name, "Date")) return kKnownHeaderDate;
                    break;
                case 'e':
                    if (detail::httpAsciiEqualsIgnoreCase(name, "ETag")) return kKnownHeaderEtag;
                    break;
                case 'v':
                    if (detail::httpAsciiEqualsIgnoreCase(name, "Vary")) return kKnownHeaderVary;
                    break;
                default:
                    break;
            }
            return 0;
        case 5:
            if (first == 'a' && detail::httpAsciiEqualsIgnoreCase(name, "Allow")) return kKnownHeaderAllow;
            return 0;
        case 6:
            if (first == 's' && detail::httpAsciiEqualsIgnoreCase(name, "Server")) return kKnownHeaderServer;
            return 0;
        case 8:
            if (first == 'l' && detail::httpAsciiEqualsIgnoreCase(name, "Location")) return kKnownHeaderLocation;
            return 0;
        case 10:
            switch (first) {
                case 'c':
                    if (detail::httpAsciiEqualsIgnoreCase(name, "Connection")) return kKnownHeaderConnection;
                    break;
                case 's':
                    if (detail::httpAsciiEqualsIgnoreCase(name, "Set-Cookie")) return kKnownHeaderSetCookie;
                    break;
                default:
                    break;
            }
            return 0;
        case 12:
            if (first == 'c' && detail::httpAsciiEqualsIgnoreCase(name, "Content-Type")) return kKnownHeaderContentType;
            return 0;
        case 13:
            switch (first) {
                case 'a':
                    if (detail::httpAsciiEqualsIgnoreCase(name, "Accept-Ranges")) return kKnownHeaderAcceptRanges;
                    break;
                case 'c':
                    if (detail::httpAsciiEqualsIgnoreCase(name, "Cache-Control")) return kKnownHeaderCacheControl;
                    if (detail::httpAsciiEqualsIgnoreCase(name, "Content-Range")) return kKnownHeaderContentRange;
                    break;
                case 'l':
                    if (detail::httpAsciiEqualsIgnoreCase(name, "Last-Modified")) return kKnownHeaderLastModified;
                    break;
                default:
                    break;
            }
            return 0;
        case 14:
            if (first == 'c' && detail::httpAsciiEqualsIgnoreCase(name, "Content-Length")) return kKnownHeaderContentLength;
            return 0;
        case 16:
            if (first == 'c' && detail::httpAsciiEqualsIgnoreCase(name, "Content-Encoding")) return kKnownHeaderContentEncoding;
            return 0;
        case 17:
            if (first == 't' && detail::httpAsciiEqualsIgnoreCase(name, "Transfer-Encoding")) return kKnownHeaderTransferEncoding;
            return 0;
        case 27:
            if (first == 'a' && detail::httpAsciiEqualsIgnoreCase(name, "Access-Control-Allow-Origin")) return kKnownHeaderAccessControlAllowOrigin;
            return 0;
        case 28:
            if (first == 'a') {
                if (detail::httpAsciiEqualsIgnoreCase(name, "Access-Control-Allow-Methods")) return kKnownHeaderAccessControlAllowMethods;
                if (detail::httpAsciiEqualsIgnoreCase(name, "Access-Control-Allow-Headers")) return kKnownHeaderAccessControlAllowHeaders;
            }
            return 0;
        case 22:
            if (first == 'a' && detail::httpAsciiEqualsIgnoreCase(name, "Access-Control-Max-Age")) return kKnownHeaderAccessControlMaxAge;
            return 0;
        case 29:
            if (first == 'a' && detail::httpAsciiEqualsIgnoreCase(name, "Access-Control-Expose-Headers")) return kKnownHeaderAccessControlExposeHeaders;
            return 0;
        case 32:
            if (first == 'a' && detail::httpAsciiEqualsIgnoreCase(name, "Access-Control-Allow-Credentials")) return kKnownHeaderAccessControlAllowCredentials;
            return 0;
        default:
            return 0;
    }
}

}  // namespace ruvia
