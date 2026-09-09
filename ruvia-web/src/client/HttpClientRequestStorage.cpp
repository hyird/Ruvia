#include "ruvia/web/detail/client/HttpClientRequestStorage.h"

#include <initializer_list>
#include <utility>

#include "ruvia/core/memory/PmrResource.h"
#include "ruvia/http/detail/util/AsciiCase.h"

namespace ruvia::detail {

HttpClientRequestStorage::HttpClientRequestStorage(
    std::string_view method, std::string_view target, std::pmr::memory_resource* resource)
    : method_(method, pmrResourceOrDefault(resource)),
      target_(target, method_.get_allocator().resource()),
      headers_(std::initializer_list<Header>{}, method_.get_allocator().resource()),
      body_(std::string_view{}, method_.get_allocator().resource()) {}

HttpClientRequestStorage::HttpClientRequestStorage(HttpClientRequestStorage&& other)
    : method_(std::move(other.method_), other.method_.get_allocator()),
      target_(std::move(other.target_), other.target_.get_allocator()),
      headers_(std::move(other.headers_), other.headers_.get_allocator()),
      body_(std::move(other.body_), other.body_.get_allocator()),
      hasBody_(other.hasBody_) {}

HttpClientRequestStorage HttpClientRequestStorage::intoResource(
    std::pmr::memory_resource* resource) && {
    auto* destinationResource = pmrResourceOrDefault(resource);
    if (method_.get_allocator().resource() == destinationResource) {
        return std::move(*this);
    }

    HttpClientRequestStorage result(method_, target_, destinationResource);
    result.headers_.reserve(headers_.size());
    for (const auto& header : headers_) {
        result.headers_.emplace_back(header.name, header.value, destinationResource);
    }
    result.body_.assign(body_);
    result.hasBody_ = hasBody_;
    return result;
}

HttpClientRequestStorage& HttpClientRequestStorage::appendHeader(
    std::string_view name, std::string_view value) {
    auto& header = headers_.emplace_back(name, value, headers_.get_allocator().resource());
    // HTTP field names are case-insensitive, but HTTP/2 requires their wire form
    // to be lowercase (RFC 9113 Section 8.2). Normalize once at the owning public
    // request boundary so the same request remains valid after ALPN selects either
    // HTTP/1.1 or HTTP/2; invalid non-token bytes are deliberately left for the
    // shared protocol validators to reject at submission time.
    for (auto& ch : header.name) {
        ch = static_cast<char>(httpAsciiToLower(static_cast<unsigned char>(ch)));
    }
    return *this;
}

HttpClientRequestStorage& HttpClientRequestStorage::setBody(std::string_view body) {
    body_.assign(body);
    hasBody_ = true;
    return *this;
}

}  // namespace ruvia::detail
