#pragma once

#include <cstdint>
#include <functional>
#include <memory_resource>
#include <optional>
#include <span>
#include <string_view>
#include <unordered_map>

#include "ruvia/core/memory/pmr_object.h"
#include "ruvia/http/quic_server.h"

#include "http3/http3_quic_datagram_bridge.h"
#include "http3/http3_quic_tls_context.h"
#include "http3/openssl_quic_crypto_provider.h"
#include "http3/openssl_quic_tls_session.h"

namespace ruvia::detail {

// Worker-affine adapter for the HTTP sans-I/O server and per-connection TLS
// sessions. HTTP owns packet classification, CID routing, Initial offers and tokens.
class http3_quic_server_transport final {
public:
    using tls_session = openssl_quic_tls_session;
    using session_owner = std::unique_ptr<tls_session, ruvia::detail::pmr_object_deleter<tls_session>>;
    struct connection_token_hash final {
        [[nodiscard]] std::size_t operator()(
            ruvia::quic_connection_token token) const noexcept {
            return std::hash<std::uint64_t>{}(token.value_);
        }
    };

    http3_quic_server_transport(http3_quic_tls_context& tls,
        ruvia::quic_server_config config = {},
        std::pmr::memory_resource* resource = std::pmr::get_default_resource());
    ~http3_quic_server_transport() noexcept;
    http3_quic_server_transport(const http3_quic_server_transport&) = delete;
    http3_quic_server_transport& operator=(const http3_quic_server_transport&) = delete;
    http3_quic_server_transport(http3_quic_server_transport&&) = delete;
    http3_quic_server_transport& operator=(http3_quic_server_transport&&) = delete;

    [[nodiscard]] ruvia::quic_server_route route_datagram(
        std::span<const std::byte> bytes, const http3_quic_datagram_address& local,
        const http3_quic_datagram_address& peer);
    // Same offer contract as ruvia::quic_server::admit_initial: only would_block
    // keeps the offer pending; a thrown failure has consumed it.
    [[nodiscard]] ruvia::quic_server_admit_result admit_initial(
        const ruvia::quic_initial_offer& offer, ruvia::quic_timestamp now,
        std::string_view server_name = {});
    [[nodiscard]] ruvia::quic_server& server() noexcept {
        return *server_;
    }
    [[nodiscard]] const ruvia::quic_server& server() const noexcept {
        return *server_;
    }
    void retire(ruvia::quic_connection_token token) noexcept;
    void stop_tls() noexcept;

private:
    std::pmr::memory_resource* resource_;
    http3_quic_tls_context& tls_context_;
    openssl_quic_crypto_provider crypto_;
    std::optional<ruvia::quic_server> server_;
    std::pmr::unordered_map<ruvia::quic_connection_token, session_owner,
        connection_token_hash>
        sessions_;
};

}  // namespace ruvia::detail
