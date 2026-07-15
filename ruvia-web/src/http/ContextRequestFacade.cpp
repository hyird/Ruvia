#include "ruvia/web/Context.h"
#include "ruvia/web/detail/http/RequestQueryValues.h"

namespace ruvia {

std::string_view ContextRequest::method() const noexcept {
    return context_->request_.method();
}

HttpKnownMethod ContextRequest::knownMethod() const noexcept {
    return context_->request_.knownMethod();
}

std::string_view ContextRequest::path() const noexcept {
    return context_->request_.path();
}

std::string_view ContextRequest::routePath() const noexcept {
    return context_->routePath_;
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

std::span<const std::string_view> ContextRequest::queries(
    std::string_view name) const {
    return context_->requestQueries().values(name);
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

const detail::ValidatedModelBindings& ContextRequest::validatedModels() const noexcept {
    return context_->validatedModels_;
}

}  // namespace ruvia
