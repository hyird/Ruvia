#pragma once

#include <cstddef>
#include <memory>
#include <span>

namespace ruvia {

namespace detail {
class HttpClientResponseState;
class HttpClientResultBudgetLease;
}  // namespace detail

// Independently owned bytes collected from an outbound HTTP response body.
// The address-stable backing storage is detached from the client and worker
// lifecycle, and its read-only view remains valid across moves until destruction.
class HttpClientResponseBytes final {
public:
    HttpClientResponseBytes(const HttpClientResponseBytes&) = delete;
    HttpClientResponseBytes& operator=(const HttpClientResponseBytes&) = delete;
    HttpClientResponseBytes(HttpClientResponseBytes&&) noexcept;
    HttpClientResponseBytes& operator=(HttpClientResponseBytes&&) noexcept;
    ~HttpClientResponseBytes();

    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] bool empty() const noexcept;
    [[nodiscard]] std::span<const std::byte> bytes() const& noexcept;
    std::span<const std::byte> bytes() const&& = delete;

private:
    friend class detail::HttpClientResponseState;

    struct Storage;

    HttpClientResponseBytes(
        std::size_t reserveBytes, detail::HttpClientResultBudgetLease&& lease);
    void append(std::span<const std::byte> bytes);

    std::unique_ptr<Storage> storage_;
};

}  // namespace ruvia
