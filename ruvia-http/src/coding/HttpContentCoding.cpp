#include "ruvia/http/detail/coding/HttpContentCoding.h"

#include <memory_resource>
#include <string_view>
#include <utility>
#include <variant>

#include "ruvia/http/detail/field/HeaderTokenUtils.h"
#include "ruvia/http/detail/coding/HttpContentCodec.h"
#include "ruvia/http/detail/util/PmrResource.h"

// The Content-Encoding field itself (RFC 9110 sections 8.4 and 5.6.1): the token
// each coding is spelled with, the list-grammar accumulator across field lines,
// and which codec a decode or encode of that coding runs on.

namespace ruvia::detail {

std::string_view httpContentCodingToken(HttpContentCoding coding) noexcept {
    switch (coding) {
        case HttpContentCoding::kBrotli:
            return "br";
        case HttpContentCoding::kZstd:
            return "zstd";
        case HttpContentCoding::kGzip:
            return "gzip";
        case HttpContentCoding::kIdentity:
            return {};
    }
    return {};
}

void HttpContentCodingFieldParser::update(std::string_view value) noexcept {
    if (std::get_if<HttpInvalidContentCodingField>(&state_) != nullptr) {
        return;
    }
    std::size_t begin = 0;
    while (begin <= value.size()) {
        const auto comma = value.find(',', begin);
        const auto token = httpTrimOws(value.substr(
            begin,
            comma == std::string_view::npos
                ? std::string_view::npos
                : comma - begin));
        if (token.empty()) {
            if (role_ == HttpFieldListRole::kSender) {
                state_.template emplace<HttpInvalidContentCodingField>();
                return;
            }
        } else if (!isValidHttpHeaderName(token)) {
            state_.template emplace<HttpInvalidContentCodingField>();
            return;
        } else if (auto* supported = std::get_if<Supported>(&state_)) {
            ++supported->codingCount;
            if (supported->codingCount > 1) {
                state_.template emplace<HttpUnsupportedContentCoding>();
            } else if (httpAsciiEqualsIgnoreCase(token, "identity")) {
                supported->coding = HttpContentCoding::kIdentity;
            } else if (httpAsciiEqualsIgnoreCase(token, "gzip") ||
                       httpAsciiEqualsIgnoreCase(token, "x-gzip")) {
                supported->coding = HttpContentCoding::kGzip;
            } else if (httpAsciiEqualsIgnoreCase(token, "br")) {
                supported->coding = HttpContentCoding::kBrotli;
            } else if (httpAsciiEqualsIgnoreCase(token, "zstd")) {
                supported->coding = HttpContentCoding::kZstd;
            } else {
                state_.template emplace<HttpUnsupportedContentCoding>();
            }
        }
        if (comma == std::string_view::npos) {
            break;
        }
        begin = comma + 1;
    }
}

HttpContentCodingFieldResult HttpContentCodingFieldParser::finish() const noexcept {
    if (std::get_if<HttpInvalidContentCodingField>(&state_) != nullptr) {
        return HttpContentCodingFieldResult(HttpInvalidContentCodingField{});
    }
    if (std::get_if<HttpUnsupportedContentCoding>(&state_) != nullptr) {
        return HttpContentCodingFieldResult(HttpUnsupportedContentCoding{});
    }
    return HttpContentCodingFieldResult(std::get<Supported>(state_).coding);
}

bool isValidHttpContentEncodingFieldValue(
    std::string_view value,
    HttpFieldListRole role) noexcept {
    HttpContentCodingFieldParser parser(role);
    parser.update(value);
    const auto result = parser.finish();
    return result.invalid() == nullptr;
}

HttpContentCodingFieldResult httpContentCodingFromFieldValue(
    std::string_view value) noexcept {
    HttpContentCodingFieldParser parser;
    parser.update(value);
    return parser.finish();
}

HttpContentDecodeResult decodeHttpContent(
    HttpContentCoding coding,
    std::string_view input,
    std::size_t maxDecodedBytes,
    std::pmr::memory_resource* resource) {
    ContentDecodeAttempt attempt = HttpContentDecodeError::kUnsupportedCoding;
    switch (coding) {
        case HttpContentCoding::kGzip:
            attempt = decodeGzipContent(
                input,
                maxDecodedBytes,
                resource);
            break;
        case HttpContentCoding::kBrotli:
            attempt = decodeBrotliContent(
                input,
                maxDecodedBytes,
                resource);
            break;
        case HttpContentCoding::kZstd:
            attempt = decodeZstdContent(
                input,
                maxDecodedBytes,
                resource);
            break;
        case HttpContentCoding::kIdentity: {
            if (input.size() > maxDecodedBytes) {
                attempt = HttpContentDecodeError::kDecodedSizeExceeded;
            } else {
                attempt = std::pmr::string(
                    input,
                    httpPmrResourceOrDefault(resource));
            }
            break;
        }
    }
    if (auto* decoded = std::get_if<std::pmr::string>(&attempt)) {
        return HttpContentDecodeResult::makeDecoded(std::move(*decoded));
    }
    return HttpContentDecodeResult::makeFailure(
        std::get<HttpContentDecodeError>(attempt));
}

HttpContentEncodeResult encodeHttpContent(
    HttpContentCoding coding,
    std::string_view input,
    std::size_t maxEncodedBytes,
    std::pmr::memory_resource* resource) {
    ContentEncodeAttempt attempt = HttpContentEncodeError::kEncoderFailure;
    switch (coding) {
        case HttpContentCoding::kBrotli:
            attempt = encodeBrotliContent(
                input,
                maxEncodedBytes,
                resource);
            break;
        case HttpContentCoding::kZstd:
            attempt = encodeZstdContent(
                input,
                maxEncodedBytes,
                resource);
            break;
        case HttpContentCoding::kGzip:
            attempt = encodeGzipContent(
                input,
                maxEncodedBytes,
                resource);
            break;
        case HttpContentCoding::kIdentity: {
            if (input.size() > maxEncodedBytes) {
                attempt = HttpContentEncodeError::kEncodedSizeExceeded;
            } else {
                attempt = std::pmr::string(
                    input,
                    httpPmrResourceOrDefault(resource));
            }
            break;
        }
    }
    if (auto* encoded = std::get_if<std::pmr::string>(&attempt)) {
        return HttpContentEncodeResult::makeEncoded(std::move(*encoded));
    }
    return HttpContentEncodeResult::makeFailure(
        std::get<HttpContentEncodeError>(attempt));
}

}  // namespace ruvia::detail
