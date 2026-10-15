#pragma once

#include <cstddef>

#include "ruvia/http/http_connection_advertisement.h"
#include "ruvia/http/http_protocol_version.h"

namespace ruvia {
namespace detail {
class http_client_advertisement_queue;
struct http_client_advertisement_state;
}  // namespace detail

// A worker-affine, independently owned observation. Its metadata survives
// client shutdown and later observations, and expires when this owner dies.
// Destroy before the event_loop retires. A slot identifies the fixed client
// pool position, not a persistent connection across reconnects.
class http_client_advertisement final {
public:
    http_client_advertisement(http_client_advertisement&& other) noexcept;
    http_client_advertisement& operator=(http_client_advertisement&& other) noexcept;
    http_client_advertisement(const http_client_advertisement&) = delete;
    http_client_advertisement& operator=(const http_client_advertisement&) = delete;
    ~http_client_advertisement();

    [[nodiscard]] http_protocol_version protocol_version() const noexcept;
    [[nodiscard]] std::size_t connection_slot() const noexcept;
    [[nodiscard]] const http_origin_advertisement* origins() const& noexcept;
    const http_origin_advertisement* origins() const&& = delete;
    [[nodiscard]] const http_alternative_service_advertisement* alternative_service() const& noexcept;
    const http_alternative_service_advertisement* alternative_service() const&& = delete;

private:
    friend class detail::http_client_advertisement_queue;
    explicit http_client_advertisement(detail::http_client_advertisement_state* state_value) noexcept
        : state_(state_value) {}
    detail::http_client_advertisement_state* state_{};
};

}  // namespace ruvia
