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

[[nodiscard]] bool fieldNameIsArray(std::string_view name) noexcept {
    return name.size() >= 2 && name.substr(name.size() - 2) == "[]";
}

void stripArraySuffix(std::pmr::string& name) {
    if (fieldNameIsArray(std::string_view(name.data(), name.size()))) {
        name.resize(name.size() - 2);
    }
}

void assignDotPath(
    Context::RequestFormField& field,
    std::pmr::memory_resource* resource) {
    field.path.clear();
    std::string_view remaining(field.name.data(), field.name.size());
    while (!remaining.empty()) {
        const auto dot = remaining.find('.');
        const auto segment = dot == std::string_view::npos
            ? remaining
            : remaining.substr(0, dot);
        field.path.emplace_back(std::pmr::string(segment.data(), segment.size(), resource));
        if (dot == std::string_view::npos) {
            break;
        }
        remaining.remove_prefix(dot + 1);
    }
}

void appendParsedBodyField(
    ContextRequest::RequestFormFieldList& fields,
    ContextRequest::RequestFormField&& field,
    ContextRequest::ParseBodyOptions options) {
    if (options.dot) {
        assignDotPath(field, fields.get_allocator().resource());
    }

    if (options.all || field.array) {
        fields.emplace_back(std::move(field));
        return;
    }

    for (auto& existing : fields) {
        if (existing.name == field.name) {
            existing = std::move(field);
            return;
        }
    }
    fields.emplace_back(std::move(field));
}

}  // namespace

const RequestNameValueList& Context::param() const {
    return routeParams();
}

const RequestNameValueList& Context::routeParams() const {
    if (routeParams_ == nullptr) {
        auto& params = memory_.emplace<RequestNameValueList>(resource());
        params.reserve(paramCount_);
        for (std::size_t i = 0; i < paramCount_; ++i) {
            params.push_back(RequestNameValueView{.name = paramNames_[i], .value = paramValues_[i]});
        }
        routeParams_ = &params;
    }
    return *routeParams_;
}

Task<std::string_view> Context::requestBody() const {
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

Task<std::pmr::vector<MultipartPart>> Context::requestMultipart() const {
    const auto boundaryValue = multipartBoundary();

    const auto requestBody = co_await this->requestBody();

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

Task<ContextRequest::RequestFormFieldList> Context::parseRequestBody(ParseBodyOptions options) const {
    const auto requestBody = co_await this->requestBody();

    if (requestContentTypeMatches("application/x-www-form-urlencoded")) {
        RequestFormFieldList fields(resource());
        fields.reserve(delimitedFieldCount(requestBody, '&'));
        bool valid = true;
        const bool ok = detail::visitUrlEncodedPairs(
            requestBody,
            [this, &fields, &valid, options](std::string_view key, std::string_view value) {
                auto decodedName = detail::decodeUrlComponentToString(key, resource(), detail::UrlDecodeMode::kForm);
                auto decodedValue = detail::decodeUrlComponentToString(value, resource(), detail::UrlDecodeMode::kForm);
                if (!decodedName || !decodedValue) {
                    valid = false;
                    return false;
                }

                const bool array = fieldNameIsArray(std::string_view(decodedName->data(), decodedName->size()));
                stripArraySuffix(*decodedName);
                appendParsedBodyField(
                    fields,
                        ContextRequest::RequestFormField(
                        resource(),
                        std::move(*decodedName),
                        std::move(*decodedValue),
                        std::pmr::string(resource()),
                        std::pmr::string(resource()),
                        false,
                        array),
                    options);
                return true;
            });
        if (!ok || !valid) {
            throw std::invalid_argument("invalid form body");
        }
        co_return fields;
    }

    if (requestContentTypeMatches("multipart/form-data")) {
        auto parts = co_await requestMultipart();
        RequestFormFieldList fields(resource());
        fields.reserve(parts.size());
        for (const auto& part : parts) {
            std::pmr::string name(part.name.data(), part.name.size(), resource());
            const bool array = fieldNameIsArray(std::string_view(name.data(), name.size()));
            stripArraySuffix(name);
            appendParsedBodyField(
                fields,
                    ContextRequest::RequestFormField(
                    resource(),
                    std::move(name),
                    std::pmr::string(part.body.data(), part.body.size(), resource()),
                    std::pmr::string(part.filename.data(), part.filename.size(), resource()),
                    std::pmr::string(part.contentType.data(), part.contentType.size(), resource()),
                    !part.filename.empty(),
                    array),
                options);
        }
        co_return fields;
    }

    throw std::invalid_argument("invalid body content type");
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
