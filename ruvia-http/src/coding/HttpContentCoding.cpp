#include "ruvia/http/detail/coding/HttpContentCoding.h"

#include <memory_resource>
#include <string_view>
#include <utility>
#include <variant>

#include "ruvia/http/HttpContentCodec.h"
#include "ruvia/http/HttpResponse.h"
#include "ruvia/http/detail/coding/HttpContentCodec.h"
#include "ruvia/http/detail/field/HeaderTokenUtils.h"
#include "ruvia/http/detail/util/PmrResource.h"

// The Content-Encoding field itself (RFC 9110 sections 8.4 and 5.6.1): the token
// each coding is spelled with, the list-grammar accumulator across field lines,
// and which codec a decode or encode of that coding runs on.

namespace ruvia {

std::string_view httpContentCodingToken(HttpContentCoding coding) noexcept {
    switch (coding) {
        case HttpContentCoding::kBrotli:
            return "br";
        case HttpContentCoding::kZstd:
            return "zstd";
        case HttpContentCoding::kGzip:
            return "gzip";
        case HttpContentCoding::kIdentity:
            return "identity";
    }
    return {};
}

}  // namespace ruvia

namespace ruvia::detail {

void HttpContentCodingFieldParser::update(std::string_view value) noexcept {
    if (std::get_if<HttpInvalidContentCodingField>(&state_) != nullptr) {
        return;
    }
    std::size_t begin = 0;
    while (begin <= value.size()) {
        const auto comma = value.find(',', begin);
        const auto token = httpTrimOws(value.substr(
            begin, comma == std::string_view::npos ? std::string_view::npos : comma - begin));
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
        return HttpContentCodingFieldResultAccess::invalid();
    }
    if (std::get_if<HttpUnsupportedContentCoding>(&state_) != nullptr) {
        return HttpContentCodingFieldResultAccess::unsupported();
    }
    return HttpContentCodingFieldResultAccess::coding(std::get<Supported>(state_).coding);
}

bool isValidHttpContentEncodingFieldValue(std::string_view value, HttpFieldListRole role) noexcept {
    HttpContentCodingFieldParser parser(role);
    parser.update(value);
    const auto result = parser.finish();
    return result.invalid() == nullptr;
}

}  // namespace ruvia::detail

namespace ruvia {

HttpContentCodingFieldResult parseHttpContentCoding(std::string_view value) noexcept {
    detail::HttpContentCodingFieldParser parser;
    parser.update(value);
    return parser.finish();
}

HttpContentCodingFieldResult parseHttpContentCodingHeaders(
    std::span<const HttpHeader> headers) noexcept {
    return detail::httpContentCodingFromHeaders(headers);
}

HttpContentCodingFieldResult parseHttpContentCodingHeaders(
    const HttpResponseHeaders& headers) noexcept {
    return detail::httpContentCodingFromHeaders(headers);
}

HttpContentDecodeResult decodeHttpContent(
    HttpContentCoding coding, std::string_view input, HttpContentDecodeOptions options) {
    switch (coding) {
        case HttpContentCoding::kGzip:
            return detail::decodeGzipContent(input, options.maxDecodedBytes, options.resource);
        case HttpContentCoding::kBrotli:
            return detail::decodeBrotliContent(input, options.maxDecodedBytes, options.resource);
        case HttpContentCoding::kZstd:
            return detail::decodeZstdContent(input, options.maxDecodedBytes, options.resource);
        case HttpContentCoding::kIdentity:
            if (input.size() > options.maxDecodedBytes) {
                return detail::HttpContentDecodeResultAccess::failure(
                    HttpContentDecodeError::kDecodedSizeExceeded);
            }
            return detail::HttpContentDecodeResultAccess::decoded(std::pmr::string(
                input, detail::httpPmrResourceOrDefault(options.resource)));
    }
    return detail::HttpContentDecodeResultAccess::failure(
        HttpContentDecodeError::kUnsupportedCoding);
}

HttpContentEncodeResult encodeHttpContent(
    HttpContentCoding coding, std::string_view input, HttpContentEncodeOptions options) {
    switch (coding) {
        case HttpContentCoding::kBrotli:
            return detail::encodeBrotliContent(input, options.maxEncodedBytes, options.resource);
        case HttpContentCoding::kZstd:
            return detail::encodeZstdContent(input, options.maxEncodedBytes, options.resource);
        case HttpContentCoding::kGzip:
            return detail::encodeGzipContent(input, options.maxEncodedBytes, options.resource);
        case HttpContentCoding::kIdentity:
            if (input.size() > options.maxEncodedBytes) {
                return detail::HttpContentEncodeResultAccess::failure(
                    HttpContentEncodeError::kEncodedSizeExceeded);
            }
            return detail::HttpContentEncodeResultAccess::encoded(std::pmr::string(
                input, detail::httpPmrResourceOrDefault(options.resource)));
    }
    return detail::HttpContentEncodeResultAccess::failure(HttpContentEncodeError::kEncoderFailure);
}

}  // namespace ruvia
