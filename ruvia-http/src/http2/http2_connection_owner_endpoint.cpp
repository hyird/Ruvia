#include "http2/http2_connection_owner_endpoint.h"

#include <exception>

#include "ruvia/http/detail/util/http_pmr_object.h"

namespace ruvia::detail {

http2_connection_owner_endpoint::http2_connection_owner_endpoint(
    void* target, abandon_request_type abandon_request, abandon_credit_type abandon_credit,
    std::pmr::memory_resource* resource) noexcept
    : target_(target),
      abandon_request_(abandon_request),
      abandon_credit_(abandon_credit),
      resource_(resource) {}

void http2_connection_owner_endpoint::retain() noexcept {
    ++references_;
}

void http2_connection_owner_endpoint::retain_storage(
    void* storage, destroy_storage_type destroy_storage) noexcept {
    if (retained_storage_ != nullptr || storage == nullptr || destroy_storage == nullptr) {
        std::terminate();
    }
    retained_storage_ = storage;
    destroy_storage_ = destroy_storage;
}

void http2_connection_owner_endpoint::release() noexcept {
    if (references_ == 0) {
        std::terminate();
    }
    if (--references_ == 0) {
        if (retained_storage_ != nullptr) {
            destroy_storage_(retained_storage_);
        }
        destroy_http_pmr_object(this, resource_);
    }
}

void http2_connection_owner_endpoint::detach() noexcept {
    target_ = nullptr;
}

void http2_connection_owner_endpoint::abandon_request(std::uint32_t stream_id) noexcept {
    if (target_ != nullptr) {
        abandon_request_(target_, stream_id);
    }
}

void http2_connection_owner_endpoint::abandon_credit(
    std::uint32_t stream_id, std::uint32_t bytes_value) noexcept {
    if (target_ != nullptr) {
        abandon_credit_(target_, stream_id, bytes_value);
    }
}

}  // namespace ruvia::detail
