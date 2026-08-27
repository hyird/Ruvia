#include "ruvia/http/detail/response/HttpResponseKnownHeaders.h"

#include "ruvia/http/detail/field/HeaderTokenUtils.h"
#include "ruvia/http/detail/response/HttpResponseHeaderBits.h"

namespace ruvia::detail {
namespace {

[[nodiscard]] unsigned char lowerAscii(unsigned char c) noexcept {
    return c >= 'A' && c <= 'Z' ? static_cast<unsigned char>(c + ('a' - 'A')) : c;
}

}  // namespace

std::uint32_t classifyResponseHeaderName(std::string_view name) noexcept {
    if (name.empty()) {
        return 0;
    }
    const auto first = lowerAscii(static_cast<unsigned char>(name.front()));
    switch (name.size()) {
        case 4:
            switch (first) {
                case 'd':
                    if (httpAsciiEqualsIgnoreCase(name, "Date")) return kResponseHeaderDate;
                    break;
                case 'e':
                    if (httpAsciiEqualsIgnoreCase(name, "ETag")) return kResponseHeaderEtag;
                    break;
                case 'v':
                    if (httpAsciiEqualsIgnoreCase(name, "Vary")) return kResponseHeaderVary;
                    break;
                default:
                    break;
            }
            return 0;
        case 5:
            if (first == 'a' && httpAsciiEqualsIgnoreCase(name, "Allow"))
                return kResponseHeaderAllow;
            return 0;
        case 6:
            if (first == 's' && httpAsciiEqualsIgnoreCase(name, "Server"))
                return kResponseHeaderServer;
            return 0;
        case 8:
            if (first == 'l' && httpAsciiEqualsIgnoreCase(name, "Location"))
                return kResponseHeaderLocation;
            return 0;
        case 10:
            switch (first) {
                case 'c':
                    if (httpAsciiEqualsIgnoreCase(name, "Connection"))
                        return kResponseHeaderConnection;
                    break;
                case 's':
                    if (httpAsciiEqualsIgnoreCase(name, "Set-Cookie"))
                        return kResponseHeaderSetCookie;
                    break;
                default:
                    break;
            }
            return 0;
        case 12:
            if (first == 'c' && httpAsciiEqualsIgnoreCase(name, "Content-Type"))
                return kResponseHeaderContentType;
            return 0;
        case 13:
            switch (first) {
                case 'a':
                    if (httpAsciiEqualsIgnoreCase(name, "Accept-Ranges"))
                        return kResponseHeaderAcceptRanges;
                    break;
                case 'c':
                    if (httpAsciiEqualsIgnoreCase(name, "Cache-Control"))
                        return kResponseHeaderCacheControl;
                    if (httpAsciiEqualsIgnoreCase(name, "Content-Range"))
                        return kResponseHeaderContentRange;
                    break;
                case 'l':
                    if (httpAsciiEqualsIgnoreCase(name, "Last-Modified"))
                        return kResponseHeaderLastModified;
                    break;
                default:
                    break;
            }
            return 0;
        case 14:
            if (first == 'c' && httpAsciiEqualsIgnoreCase(name, "Content-Length"))
                return kResponseHeaderContentLength;
            return 0;
        case 16:
            if (first == 'c' && httpAsciiEqualsIgnoreCase(name, "Content-Encoding"))
                return kResponseHeaderContentEncoding;
            return 0;
        case 17:
            if (first == 't' && httpAsciiEqualsIgnoreCase(name, "Transfer-Encoding"))
                return kResponseHeaderTransferEncoding;
            return 0;
        case 27:
            if (first == 'a' && httpAsciiEqualsIgnoreCase(name, "Access-Control-Allow-Origin"))
                return kResponseHeaderAccessControlAllowOrigin;
            return 0;
        case 28:
            if (first == 'a') {
                if (httpAsciiEqualsIgnoreCase(name, "Access-Control-Allow-Methods"))
                    return kResponseHeaderAccessControlAllowMethods;
                if (httpAsciiEqualsIgnoreCase(name, "Access-Control-Allow-Headers"))
                    return kResponseHeaderAccessControlAllowHeaders;
            }
            return 0;
        case 22:
            if (first == 'a' && httpAsciiEqualsIgnoreCase(name, "Access-Control-Max-Age"))
                return kResponseHeaderAccessControlMaxAge;
            return 0;
        case 29:
            if (first == 'a' && httpAsciiEqualsIgnoreCase(name, "Access-Control-Expose-Headers"))
                return kResponseHeaderAccessControlExposeHeaders;
            return 0;
        case 32:
            if (first == 'a' && httpAsciiEqualsIgnoreCase(name, "Access-Control-Allow-Credentials"))
                return kResponseHeaderAccessControlAllowCredentials;
            return 0;
        default:
            return 0;
    }
}

}  // namespace ruvia::detail
