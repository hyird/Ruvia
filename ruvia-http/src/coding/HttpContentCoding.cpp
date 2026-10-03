#include "ruvia/http/detail/coding/HttpContentCoding.h"

#include <memory_resource>
#include <string_view>
#include <utility>

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
        case HttpContentCoding::deflate:
            return "deflate";
        case HttpContentCoding::kIdentity:
            return "identity";
    }
    return {};
}

}  // namespace ruvia

namespace ruvia::detail {

void HttpContentCodingFieldParser::update(std::string_view value) {
    if (invalid_) {
        return;
    }
    std::size_t begin = 0;
    while (begin <= value.size()) {
        const auto comma = value.find(',', begin);
        const auto token = httpTrimOws(value.substr(
            begin, comma == std::string_view::npos ? std::string_view::npos : comma - begin));
        if (token.empty()) {
            if (role_ == HttpFieldListRole::kSender) {
                invalid_ = true;
                return;
            }
        } else if (!isValidHttpHeaderName(token)) {
            invalid_ = true;
            return;
        } else {
            HttpContentCoding coding = HttpContentCoding::kIdentity;
            if (httpAsciiEqualsIgnoreCase(token, "identity")) {
                coding = HttpContentCoding::kIdentity;
            } else if (httpAsciiEqualsIgnoreCase(token, "gzip") ||
                       httpAsciiEqualsIgnoreCase(token, "x-gzip")) {
                coding = HttpContentCoding::kGzip;
            } else if (httpAsciiEqualsIgnoreCase(token, "deflate")) {
                coding = HttpContentCoding::deflate;
            } else if (httpAsciiEqualsIgnoreCase(token, "br")) {
                coding = HttpContentCoding::kBrotli;
            } else if (httpAsciiEqualsIgnoreCase(token, "zstd")) {
                coding = HttpContentCoding::kZstd;
            } else {
                unsupported_ = true;
                if (comma == std::string_view::npos) {
                    break;
                }
                begin = comma + 1;
                continue;
            }
            codings_.push_back(coding);
        }
        if (comma == std::string_view::npos) {
            break;
        }
        begin = comma + 1;
    }
}

HttpContentCodingFieldResult HttpContentCodingFieldParser::finish() && {
    if (invalid_) {
        return HttpContentCodingFieldResultAccess::invalid();
    }
    if (unsupported_) {
        return HttpContentCodingFieldResultAccess::unsupported();
    }
    return HttpContentCodingFieldResultAccess::coding(std::move(codings_));
}

bool isValidHttpContentEncodingFieldValue(std::string_view value, HttpFieldListRole role) noexcept {
    std::size_t begin = 0;
    while (begin <= value.size()) {
        const auto comma = value.find(',', begin);
        const auto token = httpTrimOws(value.substr(
            begin, comma == std::string_view::npos ? std::string_view::npos : comma - begin));
        if (token.empty() && role == HttpFieldListRole::kSender) {
            return false;
        }
        if (!token.empty() && !isValidHttpHeaderName(token)) {
            return false;
        }
        if (comma == std::string_view::npos) {
            return true;
        }
        begin = comma + 1;
    }
    return true;
}

}  // namespace ruvia::detail

namespace ruvia {

HttpContentCodingFieldResult parseHttpContentCoding(
    std::string_view value, std::pmr::memory_resource* resource) {
    detail::HttpContentCodingFieldParser parser(detail::HttpFieldListRole::kRecipient, resource);
    parser.update(value);
    return std::move(parser).finish();
}

HttpContentCodingFieldResult parseHttpContentCodingHeaders(
    std::span<const HttpHeader> headers, std::pmr::memory_resource* resource) {
    return detail::httpContentCodingFromHeaders(headers, resource);
}

HttpContentCodingFieldResult parseHttpContentCodingHeaders(
    const HttpResponseHeaders& headers, std::pmr::memory_resource* resource) {
    return detail::httpContentCodingFromHeaders(headers, resource);
}

HttpContentDecodeResult decodeHttpContent(
    HttpContentCoding coding, std::string_view input, HttpContentDecodeOptions options) {
    switch (coding) {
        case HttpContentCoding::kGzip:
            return detail::decodeGzipContent(input, options.maxDecodedBytes, options.resource);
        case HttpContentCoding::deflate:
            return detail::decode_deflate_content(input, options.maxDecodedBytes, options.resource);
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
        case HttpContentCoding::deflate:
            return detail::encode_deflate_content(input, options.maxEncodedBytes, options.resource);
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

HttpContentEncodeResult encodeHttpContent(std::span<const HttpContentCoding> codings,
    std::string_view input, HttpContentEncodeOptions options) {
    auto* const resource = detail::httpPmrResourceOrDefault(options.resource);
    if (codings.empty()) {
        if (input.size() > options.maxEncodedBytes) {
            return detail::HttpContentEncodeResultAccess::failure(
                HttpContentEncodeError::kEncodedSizeExceeded);
        }
        return detail::HttpContentEncodeResultAccess::encoded(std::pmr::string(input, resource));
    }

    std::pmr::string current(resource);
    std::string_view source = input;
    for (const auto coding : codings) {
        auto encoded = encodeHttpContent(coding, source, options);
        auto* content = encoded.encoded();
        if (content == nullptr) {
            return detail::HttpContentEncodeResultAccess::failure(encoded.failure()->error());
        }
        current = std::move(*content).takeBytes();
        source = current;
    }
    return detail::HttpContentEncodeResultAccess::encoded(std::move(current));
}

HttpContentDecodeResult decodeHttpContent(std::span<const HttpContentCoding> codings,
    std::string_view input, HttpContentDecodeOptions options) {
    auto* const resource = detail::httpPmrResourceOrDefault(options.resource);
    if (codings.empty()) {
        if (input.size() > options.maxDecodedBytes) {
            return detail::HttpContentDecodeResultAccess::failure(
                HttpContentDecodeError::kDecodedSizeExceeded);
        }
        return detail::HttpContentDecodeResultAccess::decoded(std::pmr::string(input, resource));
    }

    std::pmr::string current(resource);
    std::string_view source = input;
    for (auto coding = codings.rbegin(); coding != codings.rend(); ++coding) {
        auto decoded = decodeHttpContent(*coding, source, options);
        auto* content = decoded.decoded();
        if (content == nullptr) {
            return detail::HttpContentDecodeResultAccess::failure(decoded.failure()->error());
        }
        current = std::move(*content).takeBytes();
        source = current;
    }
    return detail::HttpContentDecodeResultAccess::decoded(std::move(current));
}

}  // namespace ruvia
