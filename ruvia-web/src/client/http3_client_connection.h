#pragma once

#ifdef RUVIA_ENABLE_HTTP3
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <asio/ip/address.hpp>
#include <asio/ssl/context.hpp>

#include "ruvia/core/operation_deadline.h"
#include "ruvia/core/stop_token.h"
#include "ruvia/core/task.h"
#include "ruvia/http/http_priority.h"

namespace ruvia::detail {
class http_client_pool;
class http_client_request_storage;
class http_client_response_state;

// The QUIC endpoint and TLS identity of one HTTP/3 connection owner.
struct http3_client_route final {
    std::string host_;
    std::uint16_t port_;
    std::string server_name_;
};

// QUIC TLS shared by one pool's HTTP/3 connections: one SSL_CTX and resumption
// tickets isolated per route, so a ticket never resumes another server name or
// dial target. Least recently used routes give way beyond the idle bound.
class http3_client_tls final {
public:
    struct ticket;
    explicit http3_client_tls(http_client_pool& pool);
    ~http3_client_tls();
    http3_client_tls(const http3_client_tls&) = delete;
    http3_client_tls& operator=(const http3_client_tls&) = delete;
    [[nodiscard]] ticket* find(const http3_client_route& route) noexcept;
    [[nodiscard]] ticket& store(const http3_client_route& route);
    // A DNS change retires the endpoints remembered for 0-RTT.
    void forget_endpoints() noexcept;

    asio::ssl::context context_;

private:
    std::vector<ticket> tickets_;
    std::size_t capacity_;
    std::uint64_t clock_{};
};

// Worker-affine UDP/TLS/QUIC session owner of one route. The pool keeps this
// owner alive until it is reclaimable, and through join() on shutdown,
// including cancellation completions of both UDP directions.
class http3_client_connection final {
public:
    http3_client_connection(http_client_pool& pool, http3_client_tls& tls, std::size_t index, std::uint64_t serial,
        std::string_view host, std::uint16_t port, std::string_view server_name);
    ~http3_client_connection();
    http3_client_connection(const http3_client_connection&) = delete;
    http3_client_connection& operator=(const http3_client_connection&) = delete;
    [[nodiscard]] task<bool> execute(const http_client_request_storage& request, bool fallback,
        const operation_deadline& timeout, const operation_deadline& acquire_timeout,
        stop_token stop, http_client_response_state& response);
    void close() noexcept;
    void retire_addresses() noexcept;
    [[nodiscard]] std::uint64_t connection_id() const noexcept;
    void shutdown() noexcept;
    [[nodiscard]] task<void> join();
    [[nodiscard]] bool cancel_by_id(std::uint64_t id) noexcept;
    void abandon(http_client_response_state& response, bool malformed) noexcept;
    void release_data(http_client_response_state& response) noexcept;
    void reprioritize(http_client_response_state& response, http_priority priority);
    [[nodiscard]] std::size_t available() const noexcept;
    [[nodiscard]] bool active() const noexcept;
    [[nodiscard]] task<void> migrate(std::string_view local_address, std::uint16_t port,
        const operation_deadline& timeout, stop_token stop);
    [[nodiscard]] task<bool> send_datagram(http_client_response_state& response, std::span<const std::byte> bytes);
    [[nodiscard]] std::uint64_t serial() const noexcept {
        return serial_;
    }
    [[nodiscard]] const http3_client_route& route() const noexcept {
        return route_;
    }
    // The current QUIC peer while a session is open.
    [[nodiscard]] std::optional<asio::ip::address> peer_address() const noexcept;
    [[nodiscard]] bool serves(std::string_view host, std::uint16_t port, std::string_view server_name) const noexcept;
    // An endpoint selection (resolve and handshake race) is in progress;
    // further requests of the route queue behind it.
    [[nodiscard]] bool connecting() const noexcept;
    // An open session without streams.
    [[nodiscard]] bool idle() const noexcept;
    // Nothing runs on or borrows this owner any longer; the pool may destroy it.
    [[nodiscard]] bool reclaimable() const noexcept;

    bool fresh_{false};
    // Requests the pool routed here that have not released it yet.
    std::size_t users_{0};
    // Order in which the owner last turned idle (larger is newer); 0 in use.
    std::uint64_t idle_order_{0};

private:
    class session;
    class address_race;
    [[nodiscard]] task<void> select_endpoint(bool allow_early,
        const operation_deadline& timeout, const operation_deadline& acquire_timeout,
        stop_token stop, http_client_response_state& response);
    http3_client_tls& tls_;
    http3_client_route route_;
    std::uint64_t serial_;
    std::unique_ptr<session> session_;
    std::unique_ptr<address_race> race_;
};
}  // namespace ruvia::detail
#endif
