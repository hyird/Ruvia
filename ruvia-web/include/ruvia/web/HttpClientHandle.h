#pragma once

#include <cstdint>
#include <memory_resource>
#include <optional>
#include <string_view>

#include <asio/ip/udp.hpp>

#include "ruvia/core/OperationOptions.h"
#include "ruvia/core/ScopedOperation.h"
#include "ruvia/http/HttpClient.h"
#include "ruvia/http/HttpClientTunnelRequestView.h"
#include "ruvia/http/HttpConnectUdp.h"
#include "ruvia/http/quic_types.h"
#include "ruvia/web/HttpClientAdvertisement.h"
#include "ruvia/web/HttpClientExchange.h"
#include "ruvia/web/HttpClientPush.h"
#include "ruvia/web/HttpClientResponse.h"
#include "ruvia/web/HttpClientTunnel.h"
#include "ruvia/web/HttpClientTunnelConfig.h"
#include "ruvia/web/HttpClientTypes.h"

namespace ruvia {

namespace detail {
class HttpClientPool;
class HttpClientRegistry;
}  // namespace detail

class Context;

class HttpClientHandle final : private detail::ScopedCapabilityNode {
public:
    HttpClientHandle(const HttpClientHandle& other);
    HttpClientHandle& operator=(const HttpClientHandle&) = delete;

    [[nodiscard]] HttpClientHandle withOptions(OperationOptions options) const;
    [[nodiscard]] ScopedOperation<HttpClientResponse> send(
        const HttpClientRequestView& request) const;
    [[nodiscard]] ScopedOperation<HttpClientExchange> openRequest(
        const HttpClientRequestView& head, HttpClientUploadConfig upload = {}) const;
    [[nodiscard]] ScopedOperation<HttpClientTunnelResult> openTunnel(const HttpClientTunnelRequestView& request, HttpClientTunnelConfig config = {}) const;
    // Negotiates RFC 9298 and returns an accepted tunnel or ordinary rejection.
    // The proxy authority comes from this registered origin. Capsule-Protocol
    // and HTTP/1 Upgrade fields are driver-owned. Use acceptedTunnel.udp().
    [[nodiscard]] ScopedOperation<HttpClientTunnelResult> openUdpTunnel(const HttpClientUdpTunnelRequestView& request, HttpClientTunnelConfig config = {}) const;
    [[nodiscard]] HttpClientStats stats() const;
    [[nodiscard]] quic_path_migration start_quic_path_migration(
        const asio::ip::udp::endpoint& local_endpoint) const;
    [[nodiscard]] std::optional<quic_path_migration> path_migration(
        std::uint64_t id) const;
    [[nodiscard]] quic_operation_status cancel_quic_path_migration(std::uint64_t id) const;
    [[nodiscard]] std::optional<HttpClientAdvertisement> nextAdvertisement() const;
    [[nodiscard]] std::optional<HttpClientPush> nextPush() const;
    [[nodiscard]] std::string_view host() const&;
    [[nodiscard]] std::string_view host() const&& = delete;
    [[nodiscard]] std::uint16_t port() const;
    [[nodiscard]] HttpScheme scheme() const;

private:
    friend class detail::HttpClientRegistry;
    friend class Context;
    friend class WebWorkerContext;
    HttpClientHandle(detail::HttpClientPool& pool, std::pmr::memory_resource* resource,
        detail::ScopedOperationScope& scope) noexcept;
    HttpClientHandle(detail::HttpClientPool& pool, std::pmr::memory_resource* resource,
        detail::ScopedOperationScope& scope, OperationOptions options) noexcept;
    static void expireCapability(detail::ScopedCapabilityNode& capability) noexcept;

    detail::HttpClientPool* pool_{nullptr};
    std::pmr::memory_resource* resource_{nullptr};
    OperationOptions options_;
};

}  // namespace ruvia
