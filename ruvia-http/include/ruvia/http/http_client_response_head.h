#pragma once

#include <cstdint>
#include <initializer_list>
#include <memory>
#include <memory_resource>
#include <span>
#include <utility>
#include <vector>

#include "ruvia/http/attributes.h"
#include "ruvia/http/detail/util/http_pmr_object.h"
#include "ruvia/http/detail/util/pmr_resource.h"
#include "ruvia/http/http_header.h"
#include "ruvia/http/http_protocol_version.h"
#include "ruvia/http/http_status.h"

namespace ruvia::detail {
struct http_client_response_head_access;
}  // namespace ruvia::detail

namespace ruvia {

// A response-side signal for an external request-body writer. The same signal
// is emitted by HTTP/1 response plans and HTTP/2 response events.
enum class http_client_request_content_signal : std::uint8_t {
    continue_value,
    exchange_complete,
};

class http_client_response_head final {
public:
    http_client_response_head(const http_client_response_head&) = delete;
    http_client_response_head& operator=(const http_client_response_head&) = delete;
    http_client_response_head(http_client_response_head&&) noexcept = default;
    http_client_response_head& operator=(http_client_response_head&&) = delete;

    [[nodiscard]] http_status_code status() const noexcept {
        return status_;
    }

    [[nodiscard]] http_protocol_version protocol_version() const noexcept {
        return protocol_version_;
    }

    [[nodiscard]] std::span<const http_header> headers() const& noexcept RUVIA_LIFETIMEBOUND {
        return *headers_;
    }
    [[nodiscard]] std::span<const http_header> headers() const&& = delete;

    // Transfers the owned parsed fields to a consumer that retains the head's
    // metadata but needs to extend the fields' lifetime independently.
    [[nodiscard]] std::pmr::vector<http_header> take_headers() && {
        return std::pmr::vector<http_header>(std::move(*headers_), headers_->get_allocator());
    }

private:
    friend struct detail::http_client_response_head_access;

    http_client_response_head(http_status_code status, http_protocol_version protocol_version,
        std::pmr::memory_resource* resource)
        : http_client_response_head(detail::http_resolved_pmr_resource_tag{}, status, protocol_version,
              detail::http_pmr_resource_or_default(resource)) {}

    http_client_response_head(detail::http_resolved_pmr_resource_tag, http_status_code status,
        http_protocol_version protocol_version, std::pmr::memory_resource* resource)
        : status_(status),
          protocol_version_(protocol_version),
          headers_(detail::make_http_pmr_object<std::pmr::vector<http_header>>(
              resource, std::initializer_list<http_header>{}, resource)) {}

    http_status_code status_;
    http_protocol_version protocol_version_;
    std::unique_ptr<std::pmr::vector<http_header>, detail::http_pmr_object_deleter<std::pmr::vector<http_header>>> headers_;
};

}  // namespace ruvia
