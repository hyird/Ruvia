#include "ruvia/http/Context.h"

#include "HeaderTokenUtils.h"
#include "HeaderAcceptUtils.h"
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

[[noreturn]] void throwInvalidQuery() {
    throw std::invalid_argument("invalid query");
}

[[noreturn]] void throwInvalidParam() {
    throw std::invalid_argument("invalid route parameter");
}

[[noreturn]] void throwInvalidHeader() {
    throw std::invalid_argument("invalid request header");
}

[[noreturn]] void throwInvalidCookie() {
    throw std::invalid_argument("invalid cookie");
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

void appendLowerAscii(std::pmr::string& output, std::string_view input) {
    for (const char ch : input) {
        auto c = static_cast<unsigned char>(ch);
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<unsigned char>(c - 'A' + 'a');
        }
        output.push_back(static_cast<char>(c));
    }
}

[[nodiscard]] bool equalsIgnoreAsciiCase(std::string_view left, std::string_view right) noexcept {
    if (left.size() != right.size()) {
        return false;
    }
    for (std::size_t i = 0; i < left.size(); ++i) {
        auto a = static_cast<unsigned char>(left[i]);
        auto b = static_cast<unsigned char>(right[i]);
        if (a >= 'A' && a <= 'Z') {
            a = static_cast<unsigned char>(a - 'A' + 'a');
        }
        if (b >= 'A' && b <= 'Z') {
            b = static_cast<unsigned char>(b - 'A' + 'a');
        }
        if (a != b) {
            return false;
        }
    }
    return true;
}

void assignUrlDecodedOrCopy(
    std::pmr::string& output,
    std::string_view input,
    detail::UrlDecodeMode mode) {
    if (detail::hasUrlEncoding(input, mode)) {
        if (detail::decodeUrlComponent(input, output, mode)) {
            return;
        }
    }
    output.assign(input.data(), input.size());
}

[[nodiscard]] bool fieldNameIsArray(std::string_view name) noexcept {
    return name.size() >= 2 && name.substr(name.size() - 2) == "[]";
}

void assignDotPath(
    ContextRequest::RequestFormField& field,
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
    std::pmr::vector<ContextRequest::RequestFormField>& fields,
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

std::string_view ContextRequest::RawRequestClone::header(std::string_view name) const noexcept {
    for (const auto& header : headers_) {
        if (equalsIgnoreAsciiCase(header.name(), name)) {
            return header.value();
        }
    }
    return {};
}

const RequestNameValueList& Context::requestHeaders() const {
    if (requestHeaders_ == nullptr) {
        const auto rawHeaders = request_.headers();
        auto& names = memory_.emplace<std::pmr::vector<std::pmr::string>>(resource());
        auto& headers = memory_.emplace<RequestNameValueList>(resource());
        names.reserve(rawHeaders.size());
        headers.reserve(rawHeaders.size());
        for (const auto& rawHeader : rawHeaders) {
            auto& name = names.emplace_back();
            name.reserve(rawHeader.name.size());
            appendLowerAscii(name, rawHeader.name);
            headers.push_back(RequestNameValueView{
                .name = std::string_view(name.data(), name.size()),
                .value = rawHeader.value});
        }
        requestHeaders_ = &headers;
    }
    return *requestHeaders_;
}

const RequestNameValueList& Context::requestQuery() const {
    if (requestQuery_ == nullptr) {
        auto& storage = memory_.emplace<std::pmr::vector<std::pmr::string>>(resource());
        storage.reserve(delimitedFieldCount(request_.queryString(), '&') * 2);
        (void)detail::visitUrlEncodedPairs(
            request_.queryString(),
            [this, &storage](std::string_view key, std::string_view value) {
                std::pmr::string decodedName(resource());
                std::pmr::string decodedValue(resource());
                assignUrlDecodedOrCopy(decodedName, key, detail::UrlDecodeMode::kForm);
                assignUrlDecodedOrCopy(decodedValue, value, detail::UrlDecodeMode::kForm);

                for (std::size_t i = 0; i + 1 < storage.size(); i += 2) {
                    if (storage[i] == decodedName) {
                        storage[i + 1] = std::move(decodedValue);
                        return true;
                    }
                }

                storage.push_back(std::move(decodedName));
                storage.push_back(std::move(decodedValue));
                return true;
            });

        auto& query = memory_.emplace<RequestNameValueList>(resource());
        query.reserve(storage.size() / 2);
        for (std::size_t i = 0; i + 1 < storage.size(); i += 2) {
            query.push_back(RequestNameValueView{
                .name = std::string_view(storage[i].data(), storage[i].size()),
                .value = std::string_view(storage[i + 1].data(), storage[i + 1].size())});
        }
        requestQueryStorage_ = &storage;
        requestQuery_ = &query;
    }
    return *requestQuery_;
}

const RequestValueGroupList& Context::requestQueries() const {
    if (requestQueries_ == nullptr) {
        const auto pairCount = delimitedFieldCount(request_.queryString(), '&');
        auto& storage = memory_.emplace<std::pmr::vector<std::pmr::string>>(resource());
        auto& groups = memory_.emplace<RequestValueGroupList>(resource());
        storage.reserve(pairCount * 2);
        groups.reserve(pairCount);
        (void)detail::visitUrlEncodedPairs(
            request_.queryString(),
            [this, &storage, &groups](std::string_view key, std::string_view value) {
                std::pmr::string decodedName(resource());
                std::pmr::string decodedValue(resource());
                assignUrlDecodedOrCopy(decodedName, key, detail::UrlDecodeMode::kForm);
                assignUrlDecodedOrCopy(decodedValue, value, detail::UrlDecodeMode::kForm);

                const auto nameIndex = storage.size();
                storage.push_back(std::move(decodedName));
                storage.push_back(std::move(decodedValue));
                const auto name = std::string_view(storage[nameIndex].data(), storage[nameIndex].size());
                const auto queryValue = std::string_view(storage[nameIndex + 1].data(), storage[nameIndex + 1].size());

                RequestValueGroup* target = nullptr;
                for (auto& group : groups) {
                    if (group.name() == name) {
                        target = &group;
                        break;
                    }
                }
                if (target == nullptr) {
                    target = &groups.emplace_back(resource(), name);
                }
                target->add(queryValue);
                return true;
            });

        requestQueriesStorage_ = &storage;
        requestQueries_ = &groups;
    }
    return *requestQueries_;
}

const std::pmr::vector<ContextRequest::MatchedRoute>& Context::requestMatchedRoutes() const {
    if (matchedRoutes_ == nullptr) {
        auto& routes = memory_.emplace<std::pmr::vector<ContextRequest::MatchedRoute>>(resource());
        if (!routePath_.empty() && routeMethod_ != HttpMethod::kUnknown) {
            const auto method = methodName(routeMethod_);
            routes.reserve(routeMiddlewareCount_ + 1);
            for (std::size_t i = 0; i < routeMiddlewareCount_; ++i) {
                routes.push_back(ContextRequest::MatchedRoute{
                    .method = method,
                    .path = routePath_,
                    .kind = ContextRequest::MatchedRouteKind::kMiddleware});
            }
            routes.push_back(ContextRequest::MatchedRoute{
                .method = method,
                .path = routePath_,
                .kind = ContextRequest::MatchedRouteKind::kHandler});
        }
        matchedRoutes_ = &routes;
    }
    return *matchedRoutes_;
}

const RequestNameValueList& Context::routeParams() const {
    if (routeParams_ == nullptr) {
        auto& storage = memory_.emplace<std::pmr::vector<std::pmr::string>>(resource());
        auto& params = memory_.emplace<RequestNameValueList>(resource());
        storage.reserve(paramCount_ * 2);
        params.reserve(paramCount_);
        for (std::size_t i = 0; i < paramCount_; ++i) {
            std::pmr::string name(paramNames_[i].data(), paramNames_[i].size(), resource());
            std::pmr::string value(resource());
            assignUrlDecodedOrCopy(value, paramValues_[i], detail::UrlDecodeMode::kPercent);
            storage.push_back(std::move(name));
            storage.push_back(std::move(value));
            const auto nameIndex = storage.size() - 2;
            params.push_back(RequestNameValueView{
                .name = std::string_view(storage[nameIndex].data(), storage[nameIndex].size()),
                .value = std::string_view(storage[nameIndex + 1].data(), storage[nameIndex + 1].size())});
        }
        routeParamStorage_ = &storage;
        routeParams_ = &params;
    }
    return *routeParams_;
}

ParamValue Context::routeParam(std::string_view name) const {
    return ParamValue(routeParams().get(name), resource());
}

bool Context::requestAccepts(std::string_view mediaType) const noexcept {
    return detail::httpAcceptsMediaType(
        detail::requestKnownHeader(request_, detail::RequestKnownHeader::kAccept),
        mediaType);
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

Task<ContextRequest::RawRequestClone> ContextRequest::cloneRawRequest() const {
    RawRequestClone clone(context_->resource());
    clone.method_ = raw().method();
    clone.target_.assign(raw().target().data(), raw().target().size());
    const auto requestUrl = url();
    clone.url_.assign(requestUrl.data(), requestUrl.size());
    clone.path_.assign(raw().path().data(), raw().path().size());
    clone.queryString_.assign(raw().queryString().data(), raw().queryString().size());
    clone.httpVersion_.assign(raw().httpVersion().data(), raw().httpVersion().size());
    clone.headers_.reserve(raw().headers().size());
    for (const auto& header : raw().headers()) {
        clone.headers_.emplace_back(context_->resource(), header.name, header.value);
    }
    const auto requestBody = co_await text();
    clone.body_.assign(requestBody.data(), requestBody.size());
    clone.remoteAddress_.assign(raw().remoteAddress().data(), raw().remoteAddress().size());
    clone.clientCertificate_.assign(raw().clientCertificate().data(), raw().clientCertificate().size());
    clone.secure_ = raw().isSecure();
    co_return std::move(clone);
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

Task<ContextRequest::RequestFormData> Context::parseRequestBody(
    ContextRequest::ParseBodyOptions options) const {
    const auto requestBody = co_await this->requestBody();

    if (requestContentTypeMatches("application/x-www-form-urlencoded")) {
        std::pmr::vector<ContextRequest::RequestFormField> fields(resource());
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
        co_return ContextRequest::RequestFormData(std::move(fields));
    }

    if (requestContentTypeMatches("multipart/form-data")) {
        auto parts = co_await requestMultipart();
        std::pmr::vector<ContextRequest::RequestFormField> fields(resource());
        fields.reserve(parts.size());
        for (const auto& part : parts) {
            std::pmr::string name(part.name.data(), part.name.size(), resource());
            const bool array = fieldNameIsArray(std::string_view(name.data(), name.size()));
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
        co_return ContextRequest::RequestFormData(std::move(fields));
    }

    throw std::invalid_argument("invalid body content type");
}

Task<void> Context::requestDiscardBody() const {
    if (bodyLoader_ != nullptr) {
        co_await bodyLoader_->discard();
        co_return;
    }
    if (bodyReader_ != nullptr) {
        while (co_await bodyReader_->read()) {}
    }
}

BodyReader& Context::requestBodyReader() const {
    if (bodyReader_ == nullptr) {
        throw std::logic_error("request body is not streamable");
    }
    return *bodyReader_;
}

MultipartReader Context::requestMultipartReader() const {
    return MultipartReader(requestBodyReader(), multipartBoundary(), resource());
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
