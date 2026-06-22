#include "ruvia/http/Context.h"

#include "HttpRequestInternal.h"
#include "MultipartParsing.h"
#include "RequestBodyLoader.h"
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

Task<std::string_view> Context::body() const {
    if (bodyLoader_ != nullptr) {
        co_return co_await bodyLoader_->readAll();
    }
    if (bodyReader_ != nullptr) {
        throw std::logic_error("streaming request body cannot be buffered");
    }
    co_return detail::requestBodyBytes(request_);
}

bool Context::requestContentTypeMatches(std::string_view expected) const noexcept {
    return detail::contentTypeMatches(
        detail::requestKnownHeader(request_, detail::RequestKnownHeader::kContentType),
        expected);
}

Task<std::pmr::vector<MultipartPart>> Context::multipart() const {
    const auto boundaryValue = multipartBoundary();

    const auto requestBody = co_await body();
    std::pmr::string boundaryLine(resource());
    std::pmr::string boundaryPrefix(resource());
    detail::httpAssignMultipartBoundaryMarkers(boundaryLine, boundaryPrefix, boundaryValue);

    std::pmr::vector<MultipartPart> parts(resource());
    auto cursor = requestBody.find(boundaryLine);
    if (cursor == std::string_view::npos) {
        throw std::invalid_argument("invalid multipart body");
    }

    cursor += boundaryLine.size();
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
        const auto nextDelimiterPrefix = requestBody.find(boundaryPrefix, cursor);
        if (nextDelimiterPrefix == std::string_view::npos) {
            throw std::invalid_argument("invalid multipart body");
        }

        MultipartPart part(resource());
        part.name.assign(partHeaders.name.data(), partHeaders.name.size());
        if (!partHeaders.filename.empty()) {
            part.filename.assign(partHeaders.filename.data(), partHeaders.filename.size());
        }
        if (!partHeaders.contentType.empty()) {
            part.contentType.assign(partHeaders.contentType.data(), partHeaders.contentType.size());
        }
        part.body = requestBody.substr(cursor, nextDelimiterPrefix - cursor);
        parts.emplace_back(std::move(part));

        cursor = nextDelimiterPrefix + boundaryPrefix.size();
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
    return SseWriter(stream(), resource());
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
