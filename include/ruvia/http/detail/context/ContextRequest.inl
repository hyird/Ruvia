#pragma once

namespace ruvia {

inline Task<std::string_view> Context::body() const {
    if (bodyLoader_ != nullptr) {
        co_return co_await bodyLoader_->readAll();
    }
    if (bodyReader_ != nullptr) {
        throw std::logic_error("streaming request body cannot be buffered");
    }
    co_return request_.body_;
}

template <typename T>
Task<T> Context::json() const {
    static_assert(JsonBody<T>::value, "JSON body type must use RUVIA_MODEL");
    if (!detail::contentTypeMatches(request_.header(HttpRequest::KnownHeader::kContentType), "application/json")) {
        throw std::invalid_argument("invalid json content type");
    }
    const auto requestBody = co_await body();
    auto parsed = JsonBody<T>::parse(requestBody, resource());
    if (!parsed) {
        throw std::invalid_argument("invalid json body");
    }
    co_return std::move(*parsed);
}

template <typename T>
Task<T> Context::form() const {
    static_assert(FormBody<T>::value, "form body type must use RUVIA_MODEL");
    if (!detail::contentTypeMatches(
            request_.header(HttpRequest::KnownHeader::kContentType),
            "application/x-www-form-urlencoded")) {
        throw std::invalid_argument("invalid form content type");
    }
    const auto requestBody = co_await body();
    auto parsed = FormBody<T>::parse(requestBody, resource());
    if (!parsed) {
        throw std::invalid_argument("invalid form body");
    }
    co_return std::move(*parsed);
}

inline Task<std::pmr::vector<MultipartPart>> Context::multipart() const {
    const auto boundaryValue = multipartBoundary();

    const auto requestBody = co_await body();
    const detail::MultipartBoundaryMarkers boundary(boundaryValue, resource());

    std::pmr::vector<MultipartPart> parts(resource());
    auto cursor = requestBody.find(boundary.line());
    if (cursor == std::string_view::npos) {
        throw std::invalid_argument("invalid multipart body");
    }

    cursor += boundary.line().size();
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
        const auto nextDelimiterPrefix = requestBody.find(boundary.prefix(), cursor);
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

        cursor = nextDelimiterPrefix + boundary.prefix().size();
    }

    co_return parts;
}

inline Task<void> Context::discardBody() const {
    if (bodyLoader_ != nullptr) {
        co_await bodyLoader_->discard();
        co_return;
    }
    if (bodyReader_ != nullptr) {
        while (co_await bodyReader_->read()) {}
    }
}

inline BodyReader& Context::bodyReader() const {
    if (bodyReader_ == nullptr) {
        throw std::logic_error("request body is not streamable");
    }
    return *bodyReader_;
}

inline MultipartReader Context::multipartReader() const {
    return MultipartReader(bodyReader(), multipartBoundary(), resource());
}

inline WebSocket& Context::webSocket() const {
    if (webSocket_ == nullptr) {
        throw std::logic_error("websocket is not available");
    }
    return *webSocket_;
}

inline ResponseStreamWriter& Context::stream() const {
    if (responseStream_ == nullptr) {
        throw std::logic_error("response body is not streamable");
    }
    return *responseStream_;
}

inline ResponseStreamWriter& Context::streamText() {
    setResponseHeaderStableView("Content-Type", "text/plain; charset=utf-8");
    return stream();
}

inline SseWriter Context::streamSSE() const {
    return SseWriter(stream(), resource());
}

inline std::string_view Context::multipartBoundary() const {
    std::string_view boundary;
    switch (detail::httpParseMultipartBoundary(
        request_.header(HttpRequest::KnownHeader::kContentType),
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
