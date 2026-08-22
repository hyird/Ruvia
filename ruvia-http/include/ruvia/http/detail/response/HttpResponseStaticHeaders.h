#pragma once

#include "ruvia/http/detail/response/HttpResponseHeaderAccess.h"
#include "ruvia/http/detail/response/HttpResponseHeaderBits.h"
#include "ruvia/http/HttpResponse.h"

#include <cstdint>
#include <optional>
#include <string_view>

namespace ruvia::detail {

template <std::size_t N>
[[nodiscard]] constexpr HttpResponseHeader staticResponseHeader(const char (&bytes)[N], std::uint32_t nameSize, std::uint32_t knownBit) noexcept {
    return makeResponseHeader(bytes, nameSize, static_cast<std::uint32_t>(N - 1 - nameSize), knownBit, false);
}

[[nodiscard]] inline std::optional<HttpResponseHeader> builtinStaticResponseHeader(std::uint32_t knownBit, std::string_view value) noexcept {
    static constexpr char kTextContentType[] = "Content-Typetext/plain; charset=UTF-8";
    static constexpr char kJsonContentType[] = "Content-Typeapplication/json";
    static constexpr char kHtmlContentType[] = "Content-Typetext/html; charset=UTF-8";
    static constexpr char kLowercaseUtf8TextContentType[] = "Content-Typetext/plain; charset=utf-8";
    static constexpr char kLowercaseUtf8JsonContentType[] = "Content-Typeapplication/json; charset=utf-8";
    static constexpr char kLowercaseUtf8HtmlContentType[] = "Content-Typetext/html; charset=utf-8";
    static constexpr char kCssContentType[] = "Content-Typetext/css; charset=utf-8";
    static constexpr char kJsContentType[] = "Content-Typetext/javascript; charset=utf-8";
    static constexpr char kEventStreamContentType[] = "Content-Typetext/event-stream";
    static constexpr char kPngContentType[] = "Content-Typeimage/png";
    static constexpr char kJpegContentType[] = "Content-Typeimage/jpeg";
    static constexpr char kGifContentType[] = "Content-Typeimage/gif";
    static constexpr char kSvgContentType[] = "Content-Typeimage/svg+xml";
    static constexpr char kWasmContentType[] = "Content-Typeapplication/wasm";
    static constexpr char kOctetStreamContentType[] = "Content-Typeapplication/octet-stream";
    static constexpr char kConnectionClose[] = "Connectionclose";
    static constexpr char kAcceptRangesBytes[] = "Accept-Rangesbytes";
    static constexpr char kContentEncodingGzip[] = "Content-Encodinggzip";
    static constexpr char kTransferEncodingChunked[] = "Transfer-Encodingchunked";
    static constexpr char kCacheControlNoStore[] = "Cache-Controlno-store";
    static constexpr char kVaryAcceptEncoding[] = "VaryAccept-Encoding";
    static constexpr char kVaryOrigin[] = "VaryOrigin";
    static constexpr char kVaryAccessControlRequestHeaders[] = "VaryAccess-Control-Request-Headers";
    static constexpr char kVaryAccessControlRequestMethod[] = "VaryAccess-Control-Request-Method";
    static constexpr char kAccessControlAllowCredentialsTrue[] = "Access-Control-Allow-Credentialstrue";

    switch (knownBit) {
        case kResponseHeaderContentType:
            if (value == "text/plain; charset=UTF-8") {
                return staticResponseHeader(kTextContentType, 12, knownBit);
            }
            if (value == "application/json") {
                return staticResponseHeader(kJsonContentType, 12, knownBit);
            }
            if (value == "text/html; charset=UTF-8") {
                return staticResponseHeader(kHtmlContentType, 12, knownBit);
            }
            if (value == "text/plain; charset=utf-8") {
                return staticResponseHeader(kLowercaseUtf8TextContentType, 12, knownBit);
            }
            if (value == "application/json; charset=utf-8") {
                return staticResponseHeader(kLowercaseUtf8JsonContentType, 12, knownBit);
            }
            if (value == "text/html; charset=utf-8") {
                return staticResponseHeader(kLowercaseUtf8HtmlContentType, 12, knownBit);
            }
            if (value == "text/css; charset=utf-8") {
                return staticResponseHeader(kCssContentType, 12, knownBit);
            }
            if (value == "text/javascript; charset=utf-8") {
                return staticResponseHeader(kJsContentType, 12, knownBit);
            }
            if (value == "text/event-stream") {
                return staticResponseHeader(kEventStreamContentType, 12, knownBit);
            }
            if (value == "image/png") {
                return staticResponseHeader(kPngContentType, 12, knownBit);
            }
            if (value == "image/jpeg") {
                return staticResponseHeader(kJpegContentType, 12, knownBit);
            }
            if (value == "image/gif") {
                return staticResponseHeader(kGifContentType, 12, knownBit);
            }
            if (value == "image/svg+xml") {
                return staticResponseHeader(kSvgContentType, 12, knownBit);
            }
            if (value == "application/wasm") {
                return staticResponseHeader(kWasmContentType, 12, knownBit);
            }
            if (value == "application/octet-stream") {
                return staticResponseHeader(kOctetStreamContentType, 12, knownBit);
            }
            return std::nullopt;
        case kResponseHeaderConnection:
            if (value == "close") {
                return staticResponseHeader(kConnectionClose, 10, knownBit);
            }
            return std::nullopt;
        case kResponseHeaderAcceptRanges:
            if (value == "bytes") {
                return staticResponseHeader(kAcceptRangesBytes, 13, knownBit);
            }
            return std::nullopt;
        case kResponseHeaderContentEncoding:
            if (value == "gzip") {
                return staticResponseHeader(kContentEncodingGzip, 16, knownBit);
            }
            return std::nullopt;
        case kResponseHeaderTransferEncoding:
            if (value == "chunked") {
                return staticResponseHeader(kTransferEncodingChunked, 17, knownBit);
            }
            return std::nullopt;
        case kResponseHeaderCacheControl:
            if (value == "no-store") {
                return staticResponseHeader(kCacheControlNoStore, 13, knownBit);
            }
            return std::nullopt;
        case kResponseHeaderVary:
            if (value == "Accept-Encoding") {
                return staticResponseHeader(kVaryAcceptEncoding, 4, knownBit);
            }
            if (value == "Origin") {
                return staticResponseHeader(kVaryOrigin, 4, knownBit);
            }
            if (value == "Access-Control-Request-Headers") {
                return staticResponseHeader(kVaryAccessControlRequestHeaders, 4, knownBit);
            }
            if (value == "Access-Control-Request-Method") {
                return staticResponseHeader(kVaryAccessControlRequestMethod, 4, knownBit);
            }
            return std::nullopt;
        case kResponseHeaderAccessControlAllowCredentials:
            if (value == "true") {
                return staticResponseHeader(kAccessControlAllowCredentialsTrue, 32, knownBit);
            }
            return std::nullopt;
        default:
            return std::nullopt;
    }
}

}  // namespace ruvia::detail
