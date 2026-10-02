#pragma once

#include <cstddef>

#include "ruvia/http/HttpConnectionAdvertisement.h"
#include "ruvia/http/HttpProtocolVersion.h"

namespace ruvia {
namespace detail {
class HttpClientAdvertisementQueue;
struct HttpClientAdvertisementState;
}  // namespace detail

// A worker-affine, independently owned observation. Its metadata survives
// client shutdown and later observations, and expires when this owner dies.
// Destroy before the EventLoop retires. A slot identifies the fixed client
// pool position, not a persistent connection across reconnects.
class HttpClientAdvertisement final {
public:
    HttpClientAdvertisement(HttpClientAdvertisement&& other) noexcept;
    HttpClientAdvertisement& operator=(HttpClientAdvertisement&& other) noexcept;
    HttpClientAdvertisement(const HttpClientAdvertisement&) = delete;
    HttpClientAdvertisement& operator=(const HttpClientAdvertisement&) = delete;
    ~HttpClientAdvertisement();

    [[nodiscard]] HttpProtocolVersion protocolVersion() const noexcept;
    [[nodiscard]] std::size_t connectionSlot() const noexcept;
    [[nodiscard]] const HttpOriginAdvertisement* origins() const& noexcept;
    const HttpOriginAdvertisement* origins() const&& = delete;
    [[nodiscard]] const HttpAlternativeServiceAdvertisement* alternativeService() const& noexcept;
    const HttpAlternativeServiceAdvertisement* alternativeService() const&& = delete;

private:
    friend class detail::HttpClientAdvertisementQueue;
    explicit HttpClientAdvertisement(detail::HttpClientAdvertisementState* state) noexcept
        : state_(state) {}
    detail::HttpClientAdvertisementState* state_{};
};

}  // namespace ruvia
