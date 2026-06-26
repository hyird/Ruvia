#include "ruvia/http/Context.h"

#include "HeaderTokenUtils.h"
#include "HttpRequestInternal.h"
#include "MultipartParsing.h"
#include "RequestBodyDecoding.h"
#include "RequestBodyLoader.h"
#include "ruvia/http/Error.h"
#include "ruvia/http/UrlEncoding.h"
#include "ruvia/http/detail/model/Parser.h"

#include <memory_resource>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace ruvia {

namespace detail {

[[noreturn]] void throwInvalidJsonContentType() {
    throw std::invalid_argument("invalid json content type");
}

[[noreturn]] void throwInvalidJsonBody() {
    throw std::invalid_argument("invalid json body");
}

[[noreturn]] void throwInvalidFormContentType() {
    throw std::invalid_argument("invalid form content type");
}

[[noreturn]] void throwInvalidFormBody() {
    throw std::invalid_argument("invalid form body");
}

}  // namespace detail

namespace {

[[nodiscard]] std::size_t delimitedFieldCount(std::string_view input, char delimiter) noexcept {
    if (input.empty()) {
        return 0;
    }

    std::size_t count = 1;
    for (const char c : input) {
        if (c == delimiter) {
            ++count;
        }
    }
    return count;
}

}  // namespace

QueryValue Context::query(std::string_view name) const {
    if (queryParams_ == nullptr && !queryLookupAttempted_) {
        queryLookupAttempted_ = true;
        return request_.query(name);
    }

    std::optional<std::string_view> result;
    for (const auto& param : queryParams()) {
        if (detail::urlComponentEquals(param.name, name, detail::UrlDecodeMode::kForm)) {
            result = param.value;
            break;
        }
    }

    return QueryValue(result, resource(), RequestValue::DecodeMode::kForm);
}

std::optional<std::string_view> Context::cookie(std::string_view name) const {
    if (cookieParams_ == nullptr && !cookieLookupAttempted_) {
        cookieLookupAttempted_ = true;
        return request_.cookie(name);
    }

    for (const auto& cookie : cookieParams()) {
        if (cookie.name == name) {
            return cookie.value;
        }
    }
    return std::nullopt;
}

const Context::RequestNameValueList& Context::queryParams() const {
    if (queryParams_ == nullptr) {
        auto& params = memory_.emplace<RequestNameValueList>(resource());
        params.reserve(delimitedFieldCount(request_.queryString(), '&'));
        (void)detail::visitUrlEncodedPairs(
            request_.queryString(),
            [&params](std::string_view key, std::string_view value) {
                params.push_back(RequestNameValueView{.name = key, .value = value});
            });
        queryParams_ = &params;
    }
    return *queryParams_;
}

const Context::RequestNameValueList& Context::cookieParams() const {
    if (cookieParams_ == nullptr) {
        auto& params = memory_.emplace<RequestNameValueList>(resource());
        const auto input = detail::requestKnownHeader(request_, detail::RequestKnownHeader::kCookie);
        params.reserve(delimitedFieldCount(input, ';'));
        detail::httpVisitSemicolonParameters(
            input,
            [&params](std::string_view key, std::string_view value) {
                params.push_back(RequestNameValueView{.name = key, .value = value});
                return true;
            });
        cookieParams_ = &params;
    }
    return *cookieParams_;
}

Task<std::string_view> Context::body() const {
    if (bodyDecoded_) {
        const auto& decoded = *decodedBody_;
        co_return std::string_view(decoded.data(), decoded.size());
    }

    std::string_view raw;
    if (bodyLoader_ != nullptr) {
        raw = co_await bodyLoader_->readAll();
    } else if (bodyReader_ != nullptr) {
        throw std::logic_error("streaming request body cannot be buffered");
    } else {
        raw = detail::requestBodyBytes(request_);
    }

    // Transparently decode a request body whose Content-Encoding we understand,
    // so handlers always see the decoded representation (RFC 9110 §8.4).
    const auto coding = detail::requestContentCoding(
        detail::requestKnownHeader(request_, detail::RequestKnownHeader::kContentEncoding));
    if (coding == detail::HttpContentCoding::kNone || raw.empty()) {
        co_return raw;
    }
    auto& decoded = decodedBody();
    decoded.clear();
    // Keep the decoded buffer in the arena (not inline SSO) so the returned view
    // survives the Context if a handler hands it to c.text(). See assignStableString.
    decoded.reserve(32);
    if (!detail::decodeRequestContentEncoding(coding, raw, decoded, detail::kMaxDecodedRequestBodyBytes)) {
        throw HttpError(400, "bad_request", "failed to decode request body");
    }
    bodyDecoded_ = true;
    co_return std::string_view(decoded.data(), decoded.size());
}

bool Context::requestContentTypeMatches(std::string_view expected) const noexcept {
    return detail::contentTypeMatches(
        detail::requestKnownHeader(request_, detail::RequestKnownHeader::kContentType),
        expected);
}

Task<std::pmr::vector<MultipartPart>> Context::multipart() const {
    const auto boundaryValue = multipartBoundary();

    const auto requestBody = co_await body();

    std::pmr::vector<MultipartPart> parts(resource());
    auto cursor = detail::httpFindMultipartBoundaryLine(requestBody, boundaryValue);
    if (cursor == std::string_view::npos) {
        throw std::invalid_argument("invalid multipart body");
    }

    cursor += detail::httpMultipartBoundaryLineSize(boundaryValue);
    for (;;) {
        if (requestBody.substr(cursor, 2) == "--") {
            break;
        }
        if (requestBody.substr(cursor, 2) != "\r\n") {
            throw std::invalid_argument("invalid multipart body");
        }
        cursor += 2;

        const auto headersEnd = requestBody.find("\r\n\r\n", cursor);
        if (headersEnd == std::string_view::npos) {
            throw std::invalid_argument("invalid multipart body");
        }

        const auto headerBlock = requestBody.substr(cursor, headersEnd - cursor);
        detail::HttpMultipartPartHeaders partHeaders;
        switch (detail::httpParseMultipartPartHeaders(headerBlock, partHeaders)) {
            case detail::HttpMultipartPartHeaderStatus::kOk:
                break;
            case detail::HttpMultipartPartHeaderStatus::kInvalidDisposition:
                throw std::invalid_argument("invalid multipart content disposition");
            case detail::HttpMultipartPartHeaderStatus::kMissingName:
                throw std::invalid_argument("invalid multipart field name");
        }

        cursor = headersEnd + 4;
        const auto nextDelimiterPrefix = detail::httpFindMultipartBoundaryPrefix(requestBody, boundaryValue, cursor);
        if (nextDelimiterPrefix == std::string_view::npos) {
            throw std::invalid_argument("invalid multipart body");
        }

        parts.emplace_back(MultipartPart{
            .name = partHeaders.name,
            .filename = partHeaders.filename,
            .contentType = partHeaders.contentType,
            .body = requestBody.substr(cursor, nextDelimiterPrefix - cursor)});

        cursor = nextDelimiterPrefix + detail::httpMultipartBoundaryPrefixSize(boundaryValue);
    }

    co_return parts;
}

Task<void> Context::discardBody() const {
    if (bodyLoader_ != nullptr) {
        co_await bodyLoader_->discard();
        co_return;
    }
    if (bodyReader_ != nullptr) {
        while (co_await bodyReader_->read()) {}
    }
}

BodyReader& Context::bodyReader() const {
    if (bodyReader_ == nullptr) {
        throw std::logic_error("request body is not streamable");
    }
    return *bodyReader_;
}

MultipartReader Context::multipartReader() const {
    return MultipartReader(bodyReader(), multipartBoundary(), resource());
}

WebSocket& Context::webSocket() const {
    if (webSocket_ == nullptr) {
        throw std::logic_error("websocket is not available");
    }
    return *webSocket_;
}

ResponseStreamWriter& Context::stream() const {
    if (responseStream_ == nullptr) {
        throw std::logic_error("response body is not streamable");
    }
    return *responseStream_;
}

ResponseStreamWriter& Context::streamText() {
    setStableResponseHeader("Content-Type", "text/plain; charset=utf-8");
    return stream();
}

SseWriter Context::streamSSE() const {
    return SseWriter(stream());
}

std::string_view Context::multipartBoundary() const {
    std::string_view boundary;
    switch (detail::httpParseMultipartBoundary(
        detail::requestKnownHeader(request_, detail::RequestKnownHeader::kContentType),
        boundary)) {
        case detail::HttpMultipartBoundaryStatus::kOk:
            return boundary;
        case detail::HttpMultipartBoundaryStatus::kInvalidContentType:
            throw std::invalid_argument("invalid multipart content type");
        case detail::HttpMultipartBoundaryStatus::kInvalidBoundary:
            throw std::invalid_argument("invalid multipart boundary");
    }
    throw std::invalid_argument("invalid multipart boundary");
}

}  // namespace ruvia
