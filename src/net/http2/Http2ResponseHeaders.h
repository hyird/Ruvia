#pragma once

#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "Http2Hpack.h"
#include "Http2StreamState.h"
#include "../server/HttpDateCache.h"
#include "../server/HttpResponseWriter.h"
#include "ruvia/http/HeaderUtils.h"
#include "ruvia/http/HttpTypes.h"

namespace ruvia::detail {

struct Http2KnownHeaderEncoding final {
    std::string_view name;
    std::uint32_t hpackNameIndex{0};
};

[[nodiscard]] inline Http2KnownHeaderEncoding http2KnownHeaderEncoding(std::uint32_t knownBit) noexcept {
    switch (knownBit) {
        case HttpResponse::kKnownHeaderContentLength:
            return {.name = "content-length", .hpackNameIndex = HpackStaticIndex::kContentLength};
        case HttpResponse::kKnownHeaderContentEncoding:
            return {.name = "content-encoding", .hpackNameIndex = HpackStaticIndex::kContentEncoding};
        case HttpResponse::kKnownHeaderContentType:
            return {.name = "content-type", .hpackNameIndex = HpackStaticIndex::kContentType};
        case HttpResponse::kKnownHeaderVary:
            return {.name = "vary", .hpackNameIndex = HpackStaticIndex::kVary};
        case HttpResponse::kKnownHeaderDate:
            return {.name = "date", .hpackNameIndex = HpackStaticIndex::kDate};
        case HttpResponse::kKnownHeaderServer:
            return {.name = "server", .hpackNameIndex = HpackStaticIndex::kServer};
        case HttpResponse::kKnownHeaderCacheControl:
            return {.name = "cache-control", .hpackNameIndex = HpackStaticIndex::kCacheControl};
        case HttpResponse::kKnownHeaderAllow:
            return {.name = "allow", .hpackNameIndex = HpackStaticIndex::kAllow};
        case HttpResponse::kKnownHeaderAccessControlAllowOrigin:
            return {
                .name = "access-control-allow-origin",
                .hpackNameIndex = HpackStaticIndex::kAccessControlAllowOrigin};
        case HttpResponse::kKnownHeaderAccessControlAllowCredentials:
            return {.name = "access-control-allow-credentials"};
        case HttpResponse::kKnownHeaderAccessControlAllowMethods:
            return {.name = "access-control-allow-methods"};
        case HttpResponse::kKnownHeaderAccessControlAllowHeaders:
            return {.name = "access-control-allow-headers"};
        case HttpResponse::kKnownHeaderAccessControlMaxAge:
            return {.name = "access-control-max-age"};
        case HttpResponse::kKnownHeaderAccessControlExposeHeaders:
            return {.name = "access-control-expose-headers"};
        case HttpResponse::kKnownHeaderAcceptRanges:
            return {.name = "accept-ranges", .hpackNameIndex = HpackStaticIndex::kAcceptRanges};
        case HttpResponse::kKnownHeaderContentRange:
            return {.name = "content-range", .hpackNameIndex = HpackStaticIndex::kContentRange};
        case HttpResponse::kKnownHeaderEtag:
            return {.name = "etag", .hpackNameIndex = HpackStaticIndex::kEtag};
        case HttpResponse::kKnownHeaderLastModified:
            return {.name = "last-modified", .hpackNameIndex = HpackStaticIndex::kLastModified};
        case HttpResponse::kKnownHeaderLocation:
            return {.name = "location", .hpackNameIndex = HpackStaticIndex::kLocation};
        case HttpResponse::kKnownHeaderSetCookie:
            return {.name = "set-cookie", .hpackNameIndex = HpackStaticIndex::kSetCookie};
        default:
            return {};
    }
}

[[nodiscard]] inline std::string_view http2LowerHeaderName(
    Http2StreamState& stream,
    std::string_view name) {
    for (std::size_t i = 0; i < name.size(); ++i) {
        const auto ch = name[i];
        const auto byte = static_cast<unsigned char>(ch);
        if (byte >= 'A' && byte <= 'Z') {
            stream.lowerNameScratch.clear();
            stream.lowerNameScratch.reserve(name.size());
            stream.lowerNameScratch.append(name.data(), i);
            stream.lowerNameScratch.push_back(static_cast<char>(httpLowerAscii(byte)));
            for (++i; i < name.size(); ++i) {
                stream.lowerNameScratch.push_back(
                    static_cast<char>(httpLowerAscii(static_cast<unsigned char>(name[i]))));
            }
            return stream.lowerNameScratch;
        }
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

inline void appendHttp2ResponseHeaders(
    Http2StreamState& stream,
    const HttpResponse& response,
    std::uint64_t autoContentLength,
    bool emitAutoContentLength = true) {
    stream.responseHeaderBlock.clear();
    HpackEncoder::encodeStatus(stream.responseHeaderBlock, response.statusCode());

    const auto policy = responseWritePolicy(response.statusCode());
    const auto knownBits = response.knownHeaderBits();
    const bool explicitContentLengthAllowed = policy.explicitContentLengthAllowed;
    bool contentLengthWritten = false;
    for (const auto& header : response.headers()) {
        if (responseHttp2ConnectionHeaderForbidden(header.knownBit, header.name())) {
            continue;
        }
        if (header.knownBit == HttpResponse::kKnownHeaderContentLength) {
            contentLengthWritten = true;
            if (responseBodyFramingHeaderForbidden(header.knownBit, explicitContentLengthAllowed, true)) {
                continue;
            }
        }
        const auto known = http2KnownHeaderEncoding(header.knownBit);
        if (known.hpackNameIndex != 0) {
            HpackEncoder::encodeHeaderWithNameIndex(
                stream.responseHeaderBlock,
                known.hpackNameIndex,
                header.value());
            continue;
        }
        HpackEncoder::encodeHeader(
            stream.responseHeaderBlock,
            known.name.empty() ? http2LowerHeaderName(stream, header.name()) : known.name,
            header.value());
    }
    if ((knownBits & HttpResponse::kKnownHeaderServer) == 0) {
        HpackEncoder::encodeHeaderWithNameIndex(stream.responseHeaderBlock, HpackStaticIndex::kServer, "ruvia");
    }
    if ((knownBits & HttpResponse::kKnownHeaderDate) == 0) {
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

}  // namespace ruvia::detail
