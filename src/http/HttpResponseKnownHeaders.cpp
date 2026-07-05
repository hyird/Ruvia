#include "HttpResponseKnownHeaders.h"

#include "HeaderTokenUtils.h"
#include "HttpResponseHeaderBits.h"

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
                    if (asciiEqualsIgnoreCase(name, "Date")) return kResponseHeaderDate;
                    break;
                case 'e':
                    if (asciiEqualsIgnoreCase(name, "ETag")) return kResponseHeaderEtag;
                    break;
                case 'v':
                    if (asciiEqualsIgnoreCase(name, "Vary")) return kResponseHeaderVary;
                    break;
                default:
                    break;
            }
            return 0;
        case 5:
            if (first == 'a' && asciiEqualsIgnoreCase(name, "Allow")) return kResponseHeaderAllow;
            return 0;
        case 6:
            if (first == 's' && asciiEqualsIgnoreCase(name, "Server")) return kResponseHeaderServer;
            return 0;
        case 8:
            if (first == 'l' && asciiEqualsIgnoreCase(name, "Location")) return kResponseHeaderLocation;
            return 0;
        case 10:
            switch (first) {
                case 'c':
                    if (asciiEqualsIgnoreCase(name, "Connection")) return kResponseHeaderConnection;
                    break;
                case 's':
                    if (asciiEqualsIgnoreCase(name, "Set-Cookie")) return kResponseHeaderSetCookie;
                    break;
                default:
                    break;
            }
            return 0;
        case 12:
            if (first == 'c' && asciiEqualsIgnoreCase(name, "Content-Type")) return kResponseHeaderContentType;
            return 0;
        case 13:
            switch (first) {
                case 'a':
                    if (asciiEqualsIgnoreCase(name, "Accept-Ranges")) return kResponseHeaderAcceptRanges;
                    break;
                case 'c':
                    if (asciiEqualsIgnoreCase(name, "Cache-Control")) return kResponseHeaderCacheControl;
                    if (asciiEqualsIgnoreCase(name, "Content-Range")) return kResponseHeaderContentRange;
                    break;
                case 'l':
                    if (asciiEqualsIgnoreCase(name, "Last-Modified")) return kResponseHeaderLastModified;
                    break;
                default:
                    break;
            }
            return 0;
        case 14:
            if (first == 'c' && asciiEqualsIgnoreCase(name, "Content-Length")) return kResponseHeaderContentLength;
            return 0;
        case 16:
            if (first == 'c' && asciiEqualsIgnoreCase(name, "Content-Encoding")) return kResponseHeaderContentEncoding;
            return 0;
        case 17:
            if (first == 't' && asciiEqualsIgnoreCase(name, "Transfer-Encoding")) return kResponseHeaderTransferEncoding;
            return 0;
        case 27:
            if (first == 'a' && asciiEqualsIgnoreCase(name, "Access-Control-Allow-Origin")) return kResponseHeaderAccessControlAllowOrigin;
            return 0;
        case 28:
            if (first == 'a') {
                if (asciiEqualsIgnoreCase(name, "Access-Control-Allow-Methods")) return kResponseHeaderAccessControlAllowMethods;
                if (asciiEqualsIgnoreCase(name, "Access-Control-Allow-Headers")) return kResponseHeaderAccessControlAllowHeaders;
            }
            return 0;
        case 22:
            if (first == 'a' && asciiEqualsIgnoreCase(name, "Access-Control-Max-Age")) return kResponseHeaderAccessControlMaxAge;
            return 0;
        case 29:
            if (first == 'a' && asciiEqualsIgnoreCase(name, "Access-Control-Expose-Headers")) return kResponseHeaderAccessControlExposeHeaders;
            return 0;
        case 32:
            if (first == 'a' && asciiEqualsIgnoreCase(name, "Access-Control-Allow-Credentials")) return kResponseHeaderAccessControlAllowCredentials;
            return 0;
        default:
            return 0;
    }
}

}  // namespace ruvia::detail
