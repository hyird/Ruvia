#include "client/http_client_request_storage.h"

#include <initializer_list>
#include <type_traits>
#include <utility>

#include "ruvia/core/memory/pmr_resource.h"
#include "ruvia/http/http_ascii.h"

#include "client/http_client_tunnel_state.h"
#include "client/http_client_upload_state.h"

namespace ruvia::detail {

http_client_request_storage::http_client_request_storage(
    std::string_view method, std::string_view target, std::pmr::memory_resource* resource)
    : method_(method, pmr_resource_or_default(resource)),
      target_(target, method_.get_allocator().resource()),
      headers_(std::initializer_list<header_type>{}, method_.get_allocator().resource()),
      body_(std::string_view{}, method_.get_allocator().resource()),
      tunnel_authority_(method_.get_allocator().resource()),
      tunnel_protocol_(method_.get_allocator().resource()) {}

http_client_request_storage::http_client_request_storage(http_client_request_storage&& other) noexcept
    : method_(std::move(other.method_)),
      target_(std::move(other.target_)),
      headers_(std::move(other.headers_)),
      body_(std::move(other.body_)),
      tunnel_authority_(std::move(other.tunnel_authority_)),
      tunnel_protocol_(std::move(other.tunnel_protocol_)),
      is_tunnel_(other.is_tunnel_),
      tunnel_(std::exchange(other.tunnel_, nullptr)),
      has_body_(std::exchange(other.has_body_, false)),
      replay_safe_(std::exchange(other.replay_safe_, false)),
      upload_(std::exchange(other.upload_, nullptr)) {
    static_assert(std::is_nothrow_move_constructible_v<decltype(method_)>);
    static_assert(std::is_nothrow_move_constructible_v<decltype(target_)>);
    static_assert(std::is_nothrow_move_constructible_v<decltype(headers_)>);
    static_assert(std::is_nothrow_move_constructible_v<decltype(body_)>);
}

http_client_request_storage http_client_request_storage::into_resource(
    std::pmr::memory_resource* resource) && {
    auto* destination_resource = pmr_resource_or_default(resource);
    if (method_.get_allocator().resource() == destination_resource) {
        return std::move(*this);
    }

    http_client_request_storage result(method_, target_, destination_resource);
    result.headers_.reserve(headers_.size());
    for (const auto& header : headers_) {
        result.headers_.emplace_back(header.name_, header.value_, destination_resource);
    }
    result.body_.assign(body_);
    result.has_body_ = has_body_;
    result.replay_safe_ = replay_safe_;
    result.upload_ = upload_;
    result.tunnel_ = tunnel_;
    result.is_tunnel_ = is_tunnel_;
    result.tunnel_authority_.assign(tunnel_authority_);
    result.tunnel_protocol_.assign(tunnel_protocol_);
    return result;
}

void http_client_request_storage::set_tunnel(std::string_view authority, std::string_view protocol) {
    tunnel_authority_.assign(authority);
    tunnel_protocol_.assign(protocol);
    is_tunnel_ = true;
}
http_client_output_queue* http_client_request_storage::output() const noexcept {
    return tunnel_ != nullptr ? &tunnel_->output_ : upload_ != nullptr ? &upload_->output_
                                                                       : nullptr;
}

http_client_request_storage& http_client_request_storage::append_header(
    std::string_view name, std::string_view value) {
    auto& header_value = headers_.emplace_back(name, value, headers_.get_allocator().resource());
    // HTTP field names are case-insensitive, but HTTP/2 requires their wire form
    // to be lowercase (RFC 9113 Section 8.2). Normalize once at the owning public
    // request boundary so the same request remains valid after ALPN selects either
    // HTTP/1.1 or HTTP/2; invalid non-token bytes are deliberately left for the
    // shared protocol validators to reject at submission time.
    for (auto& ch : header_value.name_) {
        ch = static_cast<char>(http_ascii_to_lower(static_cast<unsigned char>(ch)));
    }
    return *this;
}

http_client_request_storage& http_client_request_storage::set_body(std::string_view body) {
    body_.assign(body);
    has_body_ = true;
    return *this;
}

http_client_request_view http_client_request_storage_access::view(
    const http_client_request_storage& request, std::pmr::vector<http_header_view>& headers) {
    headers.clear();
    headers.reserve(request.headers_.size());
    for (const auto& header : request.headers_) {
        headers.emplace_back(header.name_, header.value_);
    }
    http_client_request_view result;
    result.method_ = request.method_;
    result.target_ = request.target_;
    result.headers_ = std::span<const http_header_view>(headers);
    result.content_ = request.has_body_ ? http_client_request_content_view::bytes(request.body_)
                                        : http_client_request_content_view::none();
    result.replay_safe_ = request.replay_safe_;
    return result;
}

}  // namespace ruvia::detail
