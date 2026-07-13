#include "ruvia/web/Context.h"

namespace ruvia {

const HttpRequest& ContextRequest::raw() const noexcept {
    return context_->request_;
}

std::string_view ContextRequest::method() const noexcept {
    return raw().method();
}

HttpKnownMethod ContextRequest::knownMethod() const noexcept {
    return raw().knownMethod();
}

std::pmr::string ContextRequest::url() const {
    const auto requestTarget = raw().target();
    std::pmr::string result(context_->resource());
    if (requestTarget.starts_with("http://") || requestTarget.starts_with("https://")) {
        result.assign(requestTarget.data(), requestTarget.size());
        return result;
    }

    const auto host = header("Host");
    if (!host || host->empty() || requestTarget.empty() || requestTarget.front() != '/') {
        result.assign(requestTarget.data(), requestTarget.size());
        return result;
    }

    result.append(context_->connInfo_.tls() != nullptr ? "https://" : "http://");
    result.append(host->data(), host->size());
    result.append(requestTarget.data(), requestTarget.size());
    return result;
}

std::string_view ContextRequest::path() const noexcept {
    return raw().path();
}

std::string_view routePath(const Context& context) noexcept {
    return context.routePath_;
}

std::span<const ContextRequest::MatchedRoute> matchedRoutes(const Context& context) {
    return context.requestMatchedRoutes();
}

std::string_view routePath(const Context& context, std::ptrdiff_t index) {
    const auto routes = matchedRoutes(context);
    if (routes.empty()) {
        return {};
    }

    auto resolved = index;
    if (resolved < 0) {
        resolved += static_cast<std::ptrdiff_t>(routes.size());
    }
    if (resolved < 0 || static_cast<std::size_t>(resolved) >= routes.size()) {
        return {};
    }
    return routes[static_cast<std::size_t>(resolved)].path;
}

std::optional<std::string_view> ContextRequest::header(std::string_view name) const {
    return context_->requestHeader(name);
}

bool ContextRequest::accepts(std::string_view mediaType) const noexcept {
    return context_->requestAccepts(mediaType);
}

std::optional<std::string_view> ContextRequest::query(std::string_view name) const {
    return context_->requestQuery(name);
}

std::optional<std::span<const std::string_view>> ContextRequest::queries(
    std::string_view name) const {
    auto values = context_->requestQueries().values(name);
    if (values.empty()) {
        return std::nullopt;
    }
    return values;
}

std::optional<std::string_view> ContextRequest::cookie(std::string_view name) const {
    return context_->requestCookie(name);
}

namespace detail {

const RequestNameValueList& requestHeaderFields(const ContextRequest& request) {
    return request.context_->requestHeaders();
}

const RequestNameValueList& requestQueryFields(const ContextRequest& request) {
    return request.context_->requestQuery();
}

const RequestNameValueList& requestCookieFields(const ContextRequest& request) {
    return request.context_->requestCookies();
}

const RequestNameValueList& requestParamFields(const ContextRequest& request) {
    return request.context_->routeParams();
}

}  // namespace detail


Task<std::string_view> ContextRequest::text() const {
    return context_->requestBody();
}

Task<std::span<const std::byte>> ContextRequest::bytes() const {
    const auto body = co_await text();
    co_return std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(body.data()),
        body.size());
}

Task<ContextRequest::RequestBlob> ContextRequest::blob() const {
    auto bytes = co_await this->bytes();
    co_return RequestBlob(bytes, header("Content-Type").value_or(std::string_view{}));
}

Task<void> ContextRequest::discardBody() const {
    return context_->requestDiscardBody();
}

Task<std::pmr::vector<MultipartPart>> ContextRequest::multipart() const {
    return context_->requestMultipart();
}

Task<ContextRequest::RequestFormData> ContextRequest::parseBody(
    ParseBodyOptions options) const {
    return context_->parseRequestBody(options);
}

BodyReader& ContextRequest::bodyReader() const {
    return context_->requestBodyReader();
}

MultipartReader ContextRequest::multipartReader() const {
    return context_->requestMultipartReader();
}

std::optional<std::string_view> ContextRequest::param(std::string_view name) const {
    return context_->routeParam(name);
}

bool ContextRequest::contentTypeMatches(
    std::string_view expected) const noexcept {
    return context_->requestContentTypeMatches(expected);
}

std::pmr::memory_resource* ContextRequest::resource() const noexcept {
    return context_->resource();
}

detail::ValidatedValueStore& ContextRequest::validatedValues() const noexcept {
    return const_cast<detail::ValidatedValueStore&>(
        context_->validatedValues_);
}

}  // namespace ruvia
