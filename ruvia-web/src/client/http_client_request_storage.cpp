#include "client/http_client_request_storage.h"

#include <type_traits>
#include <utility>

#include "ruvia/core/memory/pmr_resource.h"

#include "client/http_client_tunnel_state.h"
#include "client/http_client_upload_state.h"

namespace ruvia::detail {

http_client_request_storage::http_client_request_storage(
    std::string_view method, std::string_view target, std::pmr::memory_resource* resource)
    : method_(method, pmr_resource_or_default(resource)),
      target_(target, method_.get_allocator().resource()),
      headers_(method_.get_allocator().resource()),
      body_(std::string_view{}, method_.get_allocator().resource()),
      tunnel_authority_(method_.get_allocator().resource()) {}

http_client_request_storage::http_client_request_storage(http_client_request_storage&& other) noexcept
    : method_(std::move(other.method_)),
      target_(std::move(other.target_)),
      headers_(std::move(other.headers_)),
      body_(std::move(other.body_)),
      tunnel_authority_(std::move(other.tunnel_authority_)),
      tunnel_kind_(other.tunnel_kind_),
      tunnel_(std::exchange(other.tunnel_, nullptr)),
      has_body_(std::exchange(other.has_body_, false)),
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
        result.headers_.push_back(http_header::copy_of(header.name(), header.value(), destination_resource));
    }
    result.body_.assign(body_);
    result.has_body_ = has_body_;
    result.upload_ = upload_;
    result.tunnel_ = tunnel_;
    result.tunnel_kind_ = tunnel_kind_;
    result.tunnel_authority_.assign(tunnel_authority_);
    return result;
}

void http_client_request_storage::set_tunnel(std::string_view authority, tunnel_kind kind) {
    tunnel_authority_.assign(authority);
    tunnel_kind_ = kind;
}
http_client_output_queue* http_client_request_storage::output() const noexcept {
    return tunnel_ != nullptr ? &tunnel_->output_ : upload_ != nullptr ? &upload_->output_
                                                                       : nullptr;
}

http_client_request_storage& http_client_request_storage::append_header(
    std::string_view name, std::string_view value) {
    // Normalize names once at the owning request boundary.
    headers_.push_back(http_header::copy_with_lowercase_name(name, value, resource()));
    return *this;
}

http_client_request_storage& http_client_request_storage::set_body(std::string_view body) {
    body_.assign(body);
    has_body_ = true;
    return *this;
}

http_client_request_view http_client_request_storage::view(std::pmr::vector<http_header_view>& headers) const {
    headers.clear();
    headers.reserve(headers_.size());
    for (const auto& header : headers_) {
        headers.emplace_back(header.name(), header.value());
    }
    http_client_request_view result;
    result.method_ = method_;
    result.target_ = target_;
    result.headers_ = std::span<const http_header_view>(headers);
    result.content_ = has_body_ ? http_client_request_content_view::bytes(body_)
                                : http_client_request_content_view::none();
    return result;
}

}  // namespace ruvia::detail
