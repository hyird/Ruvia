#pragma once

#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <string>
#include <string_view>

#include "../../http/HttpResponseHeaderAccess.h"
#include "../../http/HttpResponseHeaderState.h"
#include "Http2Hpack.h"
#include "Http2StreamState.h"
#include "../server/HttpDateCache.h"
#include "../server/HttpResponseHeadPolicy.h"
#include "../../http/HeaderTokenUtils.h"
#include "ruvia/http/HttpTypes.h"
#include "ruvia/http/detail/PmrString.h"

namespace ruvia::detail {

struct Http2KnownHeaderEncoding final {
    std::string_view name;
    std::uint32_t hpackNameIndex{0};
};

inline constexpr std::size_t kHttp2LowerHeaderStackBytes = 64;

[[nodiscard]] inline Http2KnownHeaderEncoding http2KnownHeaderEncoding(std::uint32_t knownBit) noexcept {
    switch (knownBit) {
        case kResponseHeaderContentLength:
            return {.name = "content-length", .hpackNameIndex = HpackStaticIndex::kContentLength};
        case kResponseHeaderContentEncoding:
            return {.name = "content-encoding", .hpackNameIndex = HpackStaticIndex::kContentEncoding};
        case kResponseHeaderContentType:
            return {.name = "content-type", .hpackNameIndex = HpackStaticIndex::kContentType};
        case kResponseHeaderVary:
            return {.name = "vary", .hpackNameIndex = HpackStaticIndex::kVary};
        case kResponseHeaderDate:
            return {.name = "date", .hpackNameIndex = HpackStaticIndex::kDate};
        case kResponseHeaderServer:
            return {.name = "server", .hpackNameIndex = HpackStaticIndex::kServer};
        case kResponseHeaderCacheControl:
            return {.name = "cache-control", .hpackNameIndex = HpackStaticIndex::kCacheControl};
        case kResponseHeaderAllow:
            return {.name = "allow", .hpackNameIndex = HpackStaticIndex::kAllow};
        case kResponseHeaderAccessControlAllowOrigin:
            return {
                .name = "access-control-allow-origin",
                .hpackNameIndex = HpackStaticIndex::kAccessControlAllowOrigin};
        case kResponseHeaderAccessControlAllowCredentials:
            return {.name = "access-control-allow-credentials"};
        case kResponseHeaderAccessControlAllowMethods:
            return {.name = "access-control-allow-methods"};
        case kResponseHeaderAccessControlAllowHeaders:
            return {.name = "access-control-allow-headers"};
        case kResponseHeaderAccessControlMaxAge:
            return {.name = "access-control-max-age"};
        case kResponseHeaderAccessControlExposeHeaders:
            return {.name = "access-control-expose-headers"};
        case kResponseHeaderAcceptRanges:
            return {.name = "accept-ranges", .hpackNameIndex = HpackStaticIndex::kAcceptRanges};
        case kResponseHeaderContentRange:
            return {.name = "content-range", .hpackNameIndex = HpackStaticIndex::kContentRange};
        case kResponseHeaderEtag:
            return {.name = "etag", .hpackNameIndex = HpackStaticIndex::kEtag};
        case kResponseHeaderLastModified:
            return {.name = "last-modified", .hpackNameIndex = HpackStaticIndex::kLastModified};
        case kResponseHeaderLocation:
            return {.name = "location", .hpackNameIndex = HpackStaticIndex::kLocation};
        case kResponseHeaderSetCookie:
            return {.name = "set-cookie", .hpackNameIndex = HpackStaticIndex::kSetCookie};
        default:
            return {};
    }
}

[[nodiscard]] inline std::string_view http2LowerHeaderName(
    std::string_view name,
    std::array<char, kHttp2LowerHeaderStackBytes>& stack,
    std::pmr::string& scratch) {
    const auto writeLower = [](std::string_view source, char* target) noexcept {
        for (std::size_t i = 0; i < source.size(); ++i) {
            target[i] = static_cast<char>(httpLowerAscii(static_cast<unsigned char>(source[i])));
        }
    };

    for (const auto ch : name) {
        const auto byte = static_cast<unsigned char>(ch);
        if (byte < 'A' || byte > 'Z') {
            continue;
        }
        if (name.size() <= stack.size()) {
            writeLower(name, stack.data());
            return std::string_view(stack.data(), name.size());
        }
        scratch.resize(name.size());
        writeLower(name, scratch.data());
        return scratch;
    }
    return name;
}

[[nodiscard]] inline std::string_view http2DateHeaderValue() noexcept {
    const auto date = cachedDateHeader();
    if (date.size() <= 8) {
        return {};
    }
    return date.substr(6, date.size() - 8);
}

[[nodiscard]] inline bool http2ResponseConnectionHeaderForbidden(
    std::uint32_t knownBit,
    std::string_view name) noexcept {
    if (knownBit == kResponseHeaderConnection ||
        knownBit == kResponseHeaderTransferEncoding) {
        return true;
    }
    if (knownBit != 0) {
        return false;
    }
    return httpAsciiEqualsIgnoreCase(name, "connection") ||
        httpAsciiEqualsIgnoreCase(name, "keep-alive") ||
        httpAsciiEqualsIgnoreCase(name, "proxy-connection") ||
        httpAsciiEqualsIgnoreCase(name, "transfer-encoding") ||
        httpAsciiEqualsIgnoreCase(name, "upgrade");
}

inline void appendHttp2ResponseHeaders(
    Http2StreamState& stream,
    const HttpResponse& response,
    std::uint64_t autoContentLength,
    bool emitAutoContentLength = true) {
    stream.responseHeaderBlock.clear();
    HpackEncoder::encodeStatus(stream.responseHeaderBlock, response.statusCode());
    std::array<char, kHttp2LowerHeaderStackBytes> lowerNameStack{};
    std::pmr::string lowerNameScratch(stream.responseHeaderBlock.get_allocator());

    const auto policy = responseWritePolicy(response.statusCode());
    const auto knownBits = responseKnownHeaderBits(response);
    const bool explicitContentLengthAllowed = policy.explicitContentLengthAllowed;
    bool contentLengthWritten = false;
    for (const auto& header : response.headers()) {
        const auto knownBit = responseHeaderKnownBit(header);
        if (http2ResponseConnectionHeaderForbidden(knownBit, header.name())) {
            continue;
        }
        if (knownBit == kResponseHeaderContentLength) {
            contentLengthWritten = true;
            if (responseBodyFramingHeaderForbidden(knownBit, explicitContentLengthAllowed, true)) {
                continue;
            }
        }
        const auto known = http2KnownHeaderEncoding(knownBit);
        if (known.hpackNameIndex != 0) {
            HpackEncoder::encodeHeaderWithNameIndex(
                stream.responseHeaderBlock,
                known.hpackNameIndex,
                header.value());
            continue;
        }
        HpackEncoder::encodeHeader(
            stream.responseHeaderBlock,
            known.name.empty()
                ? http2LowerHeaderName(header.name(), lowerNameStack, lowerNameScratch)
                : known.name,
            header.value());
    }
    if ((knownBits & kResponseHeaderServer) == 0) {
        HpackEncoder::encodeHeaderWithNameIndex(stream.responseHeaderBlock, HpackStaticIndex::kServer, "ruvia");
    }
    if ((knownBits & kResponseHeaderDate) == 0) {
        HpackEncoder::encodeHeaderWithNameIndex(
            stream.responseHeaderBlock,
            HpackStaticIndex::kDate,
            http2DateHeaderValue());
    }
    if (emitAutoContentLength && !contentLengthWritten && policy.autoContentLengthAllowed) {
        std::array<char, 20> buffer{};
        const auto [ptr, ec] = std::to_chars(buffer.data(), buffer.data() + buffer.size(), autoContentLength);
        if (ec == std::errc{}) {
            HpackEncoder::encodeHeaderWithNameIndex(
                stream.responseHeaderBlock,
                HpackStaticIndex::kContentLength,
                std::string_view(buffer.data(), static_cast<std::size_t>(ptr - buffer.data())));
        }
    }
}

inline void http2ReleaseResponseHeaderBlock(Http2StreamState& stream) {
    clearPmrStringRetainingSmall(stream.responseHeaderBlock);
}

}  // namespace ruvia::detail
