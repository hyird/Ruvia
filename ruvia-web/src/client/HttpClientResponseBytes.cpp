#include "ruvia/web/HttpClientResponseBytes.h"

#include <memory>
#include <memory_resource>
#include <utility>
#include <vector>

#include "ruvia/web/detail/client/HttpClientResultBudget.h"

namespace ruvia {

struct HttpClientResponseBytes::Storage final {
    explicit Storage(detail::HttpClientResultBudgetLease&& reservation)
        : lease(std::move(reservation)),
          bytes(std::pmr::new_delete_resource()) {}

    // Members are destroyed in reverse declaration order: free the owned byte
    // buffer before making its reservation available to another result.
    detail::HttpClientResultBudgetLease lease;
    std::pmr::vector<std::byte> bytes;
};

HttpClientResponseBytes::HttpClientResponseBytes(
    std::size_t reserveBytes, detail::HttpClientResultBudgetLease&& lease)
    : storage_(std::make_unique<Storage>(std::move(lease))) {
    storage_->bytes.reserve(reserveBytes);
}

HttpClientResponseBytes::HttpClientResponseBytes(HttpClientResponseBytes&&) noexcept = default;

HttpClientResponseBytes& HttpClientResponseBytes::operator=(HttpClientResponseBytes&&) noexcept =
    default;

HttpClientResponseBytes::~HttpClientResponseBytes() = default;

std::size_t HttpClientResponseBytes::size() const noexcept {
    return storage_ == nullptr ? 0 : storage_->bytes.size();
}

bool HttpClientResponseBytes::empty() const noexcept {
    return size() == 0;
}

std::span<const std::byte> HttpClientResponseBytes::bytes() const& noexcept {
    if (storage_ == nullptr) {
        return {};
    }
    return {storage_->bytes.data(), storage_->bytes.size()};
}

void HttpClientResponseBytes::append(std::span<const std::byte> bytes) {
    storage_->bytes.insert(storage_->bytes.end(), bytes.begin(), bytes.end());
}

}  // namespace ruvia
