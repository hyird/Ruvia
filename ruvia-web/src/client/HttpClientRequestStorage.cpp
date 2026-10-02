#include "ruvia/web/detail/client/HttpClientRequestStorage.h"

#include <initializer_list>
#include <type_traits>
#include <utility>

#include "ruvia/core/memory/PmrResource.h"
#include "ruvia/http/HttpAscii.h"
#include "ruvia/web/detail/client/HttpClientTunnelState.h"
#include "ruvia/web/detail/client/HttpClientUploadState.h"

namespace ruvia::detail {

HttpClientRequestStorage::HttpClientRequestStorage(
    std::string_view method, std::string_view target, std::pmr::memory_resource* resource)
    : method_(method, pmrResourceOrDefault(resource)),
      target_(target, method_.get_allocator().resource()),
      headers_(std::initializer_list<Header>{}, method_.get_allocator().resource()),
      body_(std::string_view{}, method_.get_allocator().resource()),
      tunnelAuthority_(method_.get_allocator().resource()),
      tunnelProtocol_(method_.get_allocator().resource()) {}

HttpClientRequestStorage::HttpClientRequestStorage(HttpClientRequestStorage&& other) noexcept
    : method_(std::move(other.method_)),
      target_(std::move(other.target_)),
      headers_(std::move(other.headers_)),
      body_(std::move(other.body_)),
      tunnelAuthority_(std::move(other.tunnelAuthority_)),
      tunnelProtocol_(std::move(other.tunnelProtocol_)),
      isTunnel_(other.isTunnel_),
      tunnel_(std::exchange(other.tunnel_, nullptr)),
      hasBody_(std::exchange(other.hasBody_, false)),
      upload_(std::exchange(other.upload_, nullptr)) {
    static_assert(std::is_nothrow_move_constructible_v<decltype(method_)>);
    static_assert(std::is_nothrow_move_constructible_v<decltype(target_)>);
    static_assert(std::is_nothrow_move_constructible_v<decltype(headers_)>);
    static_assert(std::is_nothrow_move_constructible_v<decltype(body_)>);
}

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
    result.upload_ = upload_;
    result.tunnel_ = tunnel_;
    result.isTunnel_ = isTunnel_;
    result.tunnelAuthority_.assign(tunnelAuthority_);
    result.tunnelProtocol_.assign(tunnelProtocol_);
    return result;
}

void HttpClientRequestStorage::setTunnel(std::string_view authority, std::string_view protocol) {
    tunnelAuthority_.assign(authority);
    tunnelProtocol_.assign(protocol);
    isTunnel_ = true;
}
HttpClientOutputQueue* HttpClientRequestStorage::output() const noexcept {
    return tunnel_ != nullptr ? static_cast<HttpClientOutputQueue*>(tunnel_) : static_cast<HttpClientOutputQueue*>(upload_);
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

HttpClientRequestView HttpClientRequestStorageAccess::view(
    const HttpClientRequestStorage& request, std::pmr::vector<HttpHeaderView>& headers) {
    headers.clear();
    headers.reserve(request.headers_.size());
    for (const auto& header : request.headers_) {
        headers.emplace_back(header.name, header.value);
    }
    HttpClientRequestView result;
    result.method = request.method_;
    result.target = request.target_;
    result.headers = std::span<const HttpHeaderView>(headers);
    result.content = request.hasBody_ ? HttpClientRequestContentView::bytes(request.body_)
                                      : HttpClientRequestContentView::none();
    return result;
}

}  // namespace ruvia::detail
