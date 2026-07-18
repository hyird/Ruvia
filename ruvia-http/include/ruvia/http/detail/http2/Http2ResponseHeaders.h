#pragma once

#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <stdexcept>
#include <string>
#include <string_view>

#include "ruvia/http/detail/HttpResponseHeaderAccess.h"
#include "ruvia/http/detail/HttpResponseHeaderState.h"
#include "ruvia/http/detail/HttpInterimResponseValidation.h"
#include "ruvia/http/detail/HttpHeaderSectionSize.h"
#include "ruvia/http/detail/http2/Http2Hpack.h"
#include "ruvia/http/detail/http2/Http2HeaderRules.h"
#include "ruvia/http/detail/http2/Http2ResponseHeadPlan.h"
#include "ruvia/http/detail/http2/Http2StreamState.h"
#include "ruvia/http/detail/server/HttpDateCache.h"
#include "ruvia/http/detail/server/HttpFinalResponseControlPlan.h"
#include "ruvia/http/detail/server/HttpResponseTrailers.h"
#include "ruvia/http/detail/HeaderTokenUtils.h"
#include "ruvia/http/HttpInterimResponse.h"
#include "ruvia/http/HttpResponse.h"
#include "ruvia/http/detail/PmrString.h"

namespace ruvia::detail {

enum class Http2InterimResponseHeaderEncodeStatus : std::uint8_t {
    kOk,
    kInvalidHeader
};

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
            target[i] = static_cast<char>(httpAsciiToLower(static_cast<unsigned char>(source[i])));
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

inline void appendHttp2EncodedResponseHeader(
    std::pmr::string& headerBlock,
    std::string_view name,
    std::string_view value,
    std::uint32_t knownBit,
    std::array<char, kHttp2LowerHeaderStackBytes>& lowerNameStack,
    std::pmr::string& lowerNameScratch) {
    const auto known = http2KnownHeaderEncoding(knownBit);
    if (known.hpackNameIndex != 0) {
        HpackEncoder::encodeHeaderWithNameIndex(
            headerBlock,
            known.hpackNameIndex,
            value,
            hpackHeaderNameIsSensitive(known.name));
        return;
    }
    HpackEncoder::encodeHeader(
        headerBlock,
        known.name.empty()
            ? http2LowerHeaderName(name, lowerNameStack, lowerNameScratch)
            : known.name,
        value);
}

[[nodiscard]] inline Http2InterimResponseHeaderEncodeStatus
validateHttp2InterimResponseHeaders(
    const HttpInterimResponseHead& response) noexcept {
    if (response.headers().size() > kMaxHttpHeaderFields) {
        return Http2InterimResponseHeaderEncodeStatus::kInvalidHeader;
    }
    const auto commonValidation = validateHttpInterimResponseHeaders(response);
    if (commonValidation !=
        HttpInterimResponseHeaderValidationStatus::kOk) {
        return Http2InterimResponseHeaderEncodeStatus::kInvalidHeader;
    }
    const auto statusToken = httpStatusCodeToken(response.status());
    HttpHeaderSectionSize sectionSize;
    if (!sectionSize.add(
            ":status",
            httpStatusCodeTokenView(statusToken))) {
        return Http2InterimResponseHeaderEncodeStatus::kInvalidHeader;
    }
    for (const auto& header : response.headers()) {
        const auto name = header.name();
        // RFC 9113 forbids connection-specific fields in HTTP/2. Common 1xx
        // content/framing and singleton validation has already run above.
        if (http2IsForbiddenResponseConnectionField(name) ||
            !sectionSize.add(name, header.value())) {
            return Http2InterimResponseHeaderEncodeStatus::kInvalidHeader;
        }
    }
    return Http2InterimResponseHeaderEncodeStatus::kOk;
}

[[nodiscard]] inline Http2InterimResponseHeaderEncodeStatus
appendHttp2InterimResponseHeaders(
    Http2StreamState& stream,
    const HttpInterimResponseHead& response) {
    if (const auto status = validateHttp2InterimResponseHeaders(response);
        status != Http2InterimResponseHeaderEncodeStatus::kOk) {
        return status;
    }

    auto& headerBlock = stream.responseHeaderBlock();
    headerBlock.clear();
    HpackEncoder::encodeStatus(headerBlock, response.status());
    std::array<char, kHttp2LowerHeaderStackBytes> lowerNameStack{};
    std::pmr::string lowerNameScratch(headerBlock.get_allocator());
    for (const auto& header : response.headers()) {
        appendHttp2EncodedResponseHeader(
            headerBlock,
            header.name(),
            header.value(),
            classifyResponseHeaderName(header.name()),
            lowerNameStack,
            lowerNameScratch);
    }
    return Http2InterimResponseHeaderEncodeStatus::kOk;
}

[[nodiscard]] inline bool appendHttp2ResponseHeaders(
    Http2StreamState& stream,
    const HttpResponse& response,
    const Http2ResponseHeadPlan& plan,
    const Http2FinalResponseControl& control) {
    // The unforgeable control alternative proves that the same submission path
    // rejected all HTTP/2 connection-specific fields before this function can
    // touch HPACK state. The encoder therefore has no silent filtering branch.
    (void)control;
    const auto knownBits = responseKnownHeaderBits(response);

    const auto statusToken =
        httpStatusCodeToken(plan.bodyPlan().responseStatus());
    HttpHeaderSectionSize sectionSize;
    if (!sectionSize.add(
            ":status",
            httpStatusCodeTokenView(statusToken))) {
        return false;
    }
    std::size_t fieldCount = 0;
    for (const auto& header : response.headers()) {
        if (responseHeaderKnownBit(header) == kResponseHeaderContentLength) {
            continue;
        }
        ++fieldCount;
        if (fieldCount > kMaxHttpHeaderFields ||
            !sectionSize.add(header.name(), header.value())) {
            return false;
        }
    }
    if ((knownBits & kResponseHeaderDate) == 0) {
        ++fieldCount;
        if (fieldCount > kMaxHttpHeaderFields ||
            !sectionSize.add("date", cachedDateValue())) {
            return false;
        }
    }
    std::array<char, 20> contentLengthBytes{};
    std::string_view contentLengthValue;
    if (const auto contentLength = plan.contentLength()) {
        const auto [end, error] = std::to_chars(
            contentLengthBytes.data(),
            contentLengthBytes.data() + contentLengthBytes.size(),
            *contentLength);
        if (error != std::errc{}) {
            return false;
        }
        contentLengthValue = std::string_view(
            contentLengthBytes.data(),
            static_cast<std::size_t>(end - contentLengthBytes.data()));
        ++fieldCount;
        if (fieldCount > kMaxHttpHeaderFields ||
            !sectionSize.add("content-length", contentLengthValue)) {
            return false;
        }
    }

    auto& headerBlock = stream.responseHeaderBlock();
    headerBlock.clear();
    HpackEncoder::encodeStatus(
        headerBlock,
        plan.bodyPlan().responseStatus());
    std::array<char, kHttp2LowerHeaderStackBytes> lowerNameStack{};
    std::pmr::string lowerNameScratch(headerBlock.get_allocator());
    for (const auto& header : response.headers()) {
        const auto knownBit = responseHeaderKnownBit(header);
        if (knownBit == kResponseHeaderContentLength) {
            // Content-Length is emitted only from the prepared plan below, so
            // HPACK cannot reinterpret raw application framing independently.
            continue;
        }
        appendHttp2EncodedResponseHeader(
            headerBlock,
            header.name(),
            header.value(),
            knownBit,
            lowerNameStack,
            lowerNameScratch);
    }
    if ((knownBits & kResponseHeaderDate) == 0) {
        HpackEncoder::encodeHeaderWithNameIndex(
            headerBlock,
            HpackStaticIndex::kDate,
            cachedDateValue());
    }
    if (!contentLengthValue.empty()) {
        HpackEncoder::encodeHeaderWithNameIndex(
            headerBlock,
            HpackStaticIndex::kContentLength,
            contentLengthValue);
    }
    return true;
}

inline void http2ReleaseResponseHeaderBlock(Http2StreamState& stream) {
    clearPmrStringRetainingSmall(stream.responseHeaderBlock());
}

inline void appendHttp2ResponseTrailers(
    std::pmr::string& trailerBlock,
    const HttpResponseTrailerSection& section) {
    for (const auto& trailer : section.fields()) {
        std::array<char, kHttp2LowerHeaderStackBytes> lowerNameStack{};
        std::pmr::string lowerNameScratch(trailerBlock.get_allocator());
        HpackEncoder::encodeHeader(
            trailerBlock,
            http2LowerHeaderName(
                trailer.name(), lowerNameStack, lowerNameScratch),
            trailer.value());
    }
}

}  // namespace ruvia::detail
