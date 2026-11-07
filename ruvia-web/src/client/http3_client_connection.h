#pragma once

#ifdef RUVIA_ENABLE_HTTP3
#include <memory>
#include <span>
#include <string>
#include <string_view>

#include "ruvia/core/operation_timeout.h"
#include "ruvia/core/stop_token.h"
#include "ruvia/core/task.h"

namespace ruvia::detail {
class http_client_pool;
class http_client_request_storage;
class http_client_response_state;

// Worker-affine UDP/TLS/QUIC session. The pool keeps this owner alive through
// join(), including cancellation completions of both UDP directions.
class http3_client_connection final {
public:
    http3_client_connection(http_client_pool& pool, std::size_t index);
    ~http3_client_connection();
    http3_client_connection(const http3_client_connection&) = delete;
    http3_client_connection& operator=(const http3_client_connection&) = delete;
    [[nodiscard]] task<bool> execute(const http_client_request_storage& request,
        std::string_view host, std::uint16_t port, bool fallback,
        const operation_timeout& timeout, const operation_timeout& acquire_timeout,
        stop_token stop, http_client_response_state& response);
    void close() noexcept;
    void shutdown() noexcept;
    [[nodiscard]] task<void> join();
    [[nodiscard]] bool cancel_by_id(std::uint64_t id) noexcept;
    void abandon(http_client_response_state& response, bool malformed) noexcept;
    void release_data(http_client_response_state& response) noexcept;
    [[nodiscard]] std::size_t available() const noexcept;
    [[nodiscard]] bool active() const noexcept;
    [[nodiscard]] task<void> migrate(std::string local_address, std::uint16_t port);
    [[nodiscard]] task<bool> send_datagram(http_client_response_state& response, std::span<const std::byte> bytes);

private:
    class session;
    class address_race;
    [[nodiscard]] task<void> select_endpoint(bool allow_early, std::string_view host, std::uint16_t port,
        const operation_timeout& timeout, const operation_timeout& acquire_timeout,
        stop_token stop, http_client_response_state& response);
    std::unique_ptr<session> session_;
    std::unique_ptr<address_race> race_;
};
}  // namespace ruvia::detail
#endif
