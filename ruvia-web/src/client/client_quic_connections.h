#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <memory_resource>
#include <optional>
#include <vector>

#include "client/http_client_config_storage.h"
#include "http3/http3_client_connection.h"

namespace ruvia::detail {

class http_client_advertisement_queue;
class http3_quic_client_tls_context;

// Owns stable QUIC slots, their TLS/body-budget resources, cancellation lookup
// and migration history. Connections are constructed only on their worker.
class client_quic_connections final {
public:
    static constexpr std::size_t requests_per_connection = ruvia::quic_limits{}.max_local_streams_ - 3;
    client_quic_connections(asio::io_context& io, const worker_handle& worker_value,
        task_scope& tasks, const http_client_config_storage& config,
        http_client_advertisement_queue& advertisements, http3_client_push_observer push,
        std::pmr::memory_resource* resource);
    ~client_quic_connections();
    client_quic_connections(const client_quic_connections&) = delete;
    client_quic_connections& operator=(const client_quic_connections&) = delete;
    [[nodiscard]] http3_client_connection& acquire(std::size_t slot);
    [[nodiscard]] std::size_t size() const noexcept {
        return connections_.size();
    }
    void request_stop() noexcept;
    // Called on the worker after the origin task_scope has joined.
    void retire() noexcept;
    void register_cancellation(std::uint64_t id, http3_client_connection* connection, std::uint64_t request);
    void unregister_cancellation(std::uint64_t id) noexcept;
    [[nodiscard]] bool cancel(std::uint64_t id) noexcept;
    [[nodiscard]] worker_signal& generation_signal() noexcept {
        return generation_signal_;
    }
    [[nodiscard]] ruvia::quic_path_migration start_path_migration(const asio::ip::udp::endpoint& endpoint);
    [[nodiscard]] std::optional<ruvia::quic_path_migration> path_migration(std::uint64_t id) const noexcept;
    [[nodiscard]] ruvia::quic_operation_status cancel_path_migration(std::uint64_t id);

private:
    struct pending_cancellation final {
        std::uint64_t id_{};
        http3_client_connection* connection_{};
        std::uint64_t request_{};
    };
    struct migration_tracking final {
        std::uint64_t id_{};
        std::size_t slot_{};
        std::uint64_t generation_{};
        std::uint64_t connection_migration_id_{};
        ruvia::quic_path_migration result_{};
    };
    asio::io_context& io_;
    const worker_handle& worker_;
    task_scope& tasks_;
    const http_client_config_storage& config_;
    http_client_advertisement_queue& advertisements_;
    http3_client_push_observer push_;
    std::pmr::memory_resource* resource_;
    std::unique_ptr<http3_quic_client_tls_context, pmr_object_deleter<http3_quic_client_tls_context>> tls_;
    std::unique_ptr<http3_client_body_budget, pmr_object_deleter<http3_client_body_budget>> body_budget_;
    std::pmr::vector<std::unique_ptr<http3_client_connection, pmr_object_deleter<http3_client_connection>>> connections_;
    worker_signal generation_signal_;
    std::pmr::vector<pending_cancellation> cancellations_;
    std::optional<migration_tracking> migration_;
    std::uint64_t next_migration_id_{1};
    bool retired_{};
};

}  // namespace ruvia::detail
