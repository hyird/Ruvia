#include "ruvia/web/http_client_response_bytes.h"

#include <memory>
#include <memory_resource>
#include <utility>
#include <vector>

#include "client/http_client_result_budget.h"

namespace ruvia {

struct http_client_response_bytes::storage_type final {
    explicit storage_type(detail::http_client_result_budget_lease&& reservation)
        : lease_(std::move(reservation)),
          bytes_(std::pmr::new_delete_resource()) {}

    // Members are destroyed in reverse declaration order: free the owned byte
    // buffer before making its reservation available to another result.
    detail::http_client_result_budget_lease lease_;
    std::pmr::vector<std::byte> bytes_;
};

http_client_response_bytes::http_client_response_bytes(
    std::size_t reserve_bytes, detail::http_client_result_budget_lease&& lease_value)
    : storage_(std::make_unique<storage_type>(std::move(lease_value))) {
    storage_->bytes_.reserve(reserve_bytes);
}

http_client_response_bytes::http_client_response_bytes(http_client_response_bytes&&) noexcept = default;

http_client_response_bytes& http_client_response_bytes::operator=(http_client_response_bytes&&) noexcept =
    default;

http_client_response_bytes::~http_client_response_bytes() = default;

std::size_t http_client_response_bytes::size() const noexcept {
    return storage_ == nullptr ? 0 : storage_->bytes_.size();
}

bool http_client_response_bytes::empty() const noexcept {
    return size() == 0;
}

std::span<const std::byte> http_client_response_bytes::bytes() const& noexcept {
    if (storage_ == nullptr) {
        return {};
    }
    return {storage_->bytes_.data(), storage_->bytes_.size()};
}

void http_client_response_bytes::append(std::span<const std::byte> bytes_value) {
    storage_->bytes_.insert(storage_->bytes_.end(), bytes_value.begin(), bytes_value.end());
}

}  // namespace ruvia
