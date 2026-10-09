#pragma once

#include <memory_resource>
#include <span>
#include <vector>

#include "ruvia/http/http_header.h"
#include "ruvia/http/http_status.h"

namespace ruvia {
namespace detail {
class http_client_response_state;
}

// Immutable metadata retained by its http_client_response. Field views borrow
// that response and remain valid through subsequent body reads.
class http_client_informational_response final {
public:
    http_client_informational_response(const http_client_informational_response&) = delete;
    http_client_informational_response& operator=(const http_client_informational_response&) = delete;
    http_client_informational_response(http_client_informational_response&&) noexcept = default;
    http_client_informational_response& operator=(http_client_informational_response&&) = delete;
    [[nodiscard]] http_status_code status() const noexcept {
        return status_;
    }
    [[nodiscard]] std::span<const http_header> headers() const& noexcept {
        return headers_;
    }
    std::span<const http_header> headers() const&& = delete;

private:
    friend class detail::http_client_response_state;
    http_client_informational_response(http_status_code status, std::pmr::memory_resource* resource)
        : status_(status),
          headers_(resource) {}
    http_status_code status_;
    std::pmr::vector<http_header> headers_;
};
}  // namespace ruvia
