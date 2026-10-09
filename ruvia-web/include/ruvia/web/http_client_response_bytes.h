#pragma once

#include <cstddef>
#include <memory>
#include <span>

namespace ruvia {

namespace detail {
class http_client_response_state;
class http_client_result_budget_lease;
}  // namespace detail

// Independently owned bytes collected from an outbound HTTP response body.
// The address-stable backing storage is detached from the client and worker
// lifecycle, and its read-only view remains valid across moves until destruction.
class http_client_response_bytes final {
public:
    http_client_response_bytes(const http_client_response_bytes&) = delete;
    http_client_response_bytes& operator=(const http_client_response_bytes&) = delete;
    http_client_response_bytes(http_client_response_bytes&&) noexcept;
    http_client_response_bytes& operator=(http_client_response_bytes&&) noexcept;
    ~http_client_response_bytes();

    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] bool empty() const noexcept;
    [[nodiscard]] std::span<const std::byte> bytes() const& noexcept;
    std::span<const std::byte> bytes() const&& = delete;

private:
    friend class detail::http_client_response_state;

    struct storage_type;

    http_client_response_bytes(
        std::size_t reserve_bytes, detail::http_client_result_budget_lease&& lease_value);
    void append(std::span<const std::byte> bytes);

    std::unique_ptr<storage_type> storage_;
};

}  // namespace ruvia
