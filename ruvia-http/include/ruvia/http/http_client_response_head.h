#pragma once

#include <cstdint>
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

// Owns response fields in the parser/connection PMR. That resource must outlive
// the head and any field vector extracted from it.
// Decoding a head without fields does not allocate field storage.
// HEAD response fields remain metadata even for status 205: Content-Length
// does not become the response's received-content length.
class http_client_response_head final {
public:
    http_client_response_head(const http_client_response_head&) = delete;
    http_client_response_head& operator=(const http_client_response_head&) = delete;
    // Moving transfers fields without allocation. The source retains its
    // status and version and exposes empty fields.
    http_client_response_head(http_client_response_head&&) noexcept = default;
    http_client_response_head& operator=(http_client_response_head&&) = delete;

    [[nodiscard]] http_status_code status() const noexcept {
        return status_;
    }

    [[nodiscard]] http_protocol_version protocol_version() const noexcept {
        return protocol_version_;
    }

    // Parsed field lines retain wire order and repeated metadata fields. In
    // particular, Content-Type values are not parsed as media types, merged,
    // or selected for the application, even when their media-type syntax is invalid.
    // Content-Encoding is also retained without interpreting coding-list syntax;
    // content-decoding APIs validate it independently of message framing.
    [[nodiscard]] std::span<const http_header> headers() const& noexcept RUVIA_LIFETIMEBOUND {
        if (headers_ == nullptr) {
            return {};
        }
        return *headers_;
    }
    [[nodiscard]] std::span<const http_header> headers() const&& = delete;

    // Transfers the owned parsed fields to a consumer that retains the head's
    // metadata but needs to extend the fields' lifetime independently.
    // Repeated extraction, including after moving the head, returns an empty
    // vector in the original PMR resource, which must still be alive.
    [[nodiscard]] std::pmr::vector<http_header> take_headers() && {
        if (headers_ == nullptr) {
            return std::pmr::vector<http_header>(headers_.get_deleter().resource_);
        }
        return std::pmr::vector<http_header>(std::move(*headers_), headers_->get_allocator());
    }

private:
    friend struct detail::http_client_response_head_access;

    http_client_response_head(http_status_code status, http_protocol_version protocol_version,
        std::pmr::memory_resource* resource) noexcept
        : http_client_response_head(detail::http_resolved_pmr_resource_tag{}, status, protocol_version,
              detail::http_pmr_resource_or_default(resource)) {}

    http_client_response_head(detail::http_resolved_pmr_resource_tag, http_status_code status,
        http_protocol_version protocol_version, std::pmr::memory_resource* resource) noexcept
        : status_(status),
          protocol_version_(protocol_version),
          headers_(nullptr, detail::http_pmr_object_deleter<std::pmr::vector<http_header>>{resource}) {}

    http_status_code status_;
    http_protocol_version protocol_version_;
    std::unique_ptr<std::pmr::vector<http_header>, detail::http_pmr_object_deleter<std::pmr::vector<http_header>>> headers_;
};

}  // namespace ruvia
