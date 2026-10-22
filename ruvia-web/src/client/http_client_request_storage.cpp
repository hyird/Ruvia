#include "client/http_client_request_storage.h"

#include <type_traits>
#include <utility>

#include "ruvia/core/memory/pmr_object.h"

#include "client/http_client_tunnel_state.h"
#include "client/http_client_upload_state.h"

namespace ruvia::detail {

http_client_output_queue* http_client_request_storage::output() const noexcept {
    return tunnel_ != nullptr ? &tunnel_->output_ : upload_ != nullptr ? &upload_->output_
                                                                       : nullptr;
}

http_client_request_storage::http_client_request_storage(std::string_view method,
    std::string_view target, std::span<const http_header_view> fields, std::pmr::memory_resource* resource)
    : http_client_request_storage(method, target, resource) {
    if (fields.empty()) {
        return;
    }
    headers_.reserve(fields.size());
    auto* const field_resource = this->resource();
    for (const auto& field : fields) {
        headers_.push_back(http_header::copy_of(field.name(), field.value(), field_resource));
    }
}

http_client_request_storage::http_client_request_storage(
    std::string_view method, std::string_view target, std::pmr::memory_resource* resource)
    : method_(method, pmr_resource_or_default(resource)),
      target_(target, method_.get_allocator().resource()),
      headers_(method_.get_allocator().resource()),
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

http_client_request_storage& http_client_request_storage::set_body(std::string_view body) {
    body_.assign(body);
    has_body_ = true;
    return *this;
}

}  // namespace ruvia::detail
