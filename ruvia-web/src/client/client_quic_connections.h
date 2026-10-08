#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <memory_resource>
#include <optional>
#include <vector>

#include "client/HttpClientConfigStorage.h"
#include "http3/Http3ClientConnection.h"

namespace ruvia::detail {

class HttpClientAdvertisementQueue;
class http3_quic_client_tls_context;

// Owns stable QUIC slots, their TLS/body-budget resources, cancellation lookup
// and migration history. Connections are constructed only on their worker.
class client_quic_connections final {
public:
    static constexpr std::size_t requests_per_connection = ruvia::quic_limits{}.max_local_streams - 3;
    client_quic_connections(asio::io_context& io, const WorkerHandle& worker,
        TaskScope& tasks, const HttpClientConfigStorage& config,
        HttpClientAdvertisementQueue& advertisements, Http3ClientPushObserver push,
        std::pmr::memory_resource* resource);
    ~client_quic_connections();
    client_quic_connections(const client_quic_connections&) = delete;
    client_quic_connections& operator=(const client_quic_connections&) = delete;
    [[nodiscard]] Http3ClientConnection& acquire(std::size_t slot);
    [[nodiscard]] std::size_t size() const noexcept {
        return connections_.size();
    }
    void request_stop() noexcept;
    // Called on the worker after the origin TaskScope has joined.
    void retire() noexcept;
    void register_cancellation(std::uint64_t id, Http3ClientConnection* connection, std::uint64_t request);
    void unregister_cancellation(std::uint64_t id) noexcept;
    [[nodiscard]] bool cancel(std::uint64_t id) noexcept;
    [[nodiscard]] WorkerSignal& generation_signal() noexcept {
        return generation_signal_;
    }
    [[nodiscard]] ruvia::quic_path_migration start_path_migration(const asio::ip::udp::endpoint& endpoint);
    [[nodiscard]] std::optional<ruvia::quic_path_migration> path_migration(std::uint64_t id) const noexcept;
    [[nodiscard]] ruvia::quic_operation_status cancel_path_migration(std::uint64_t id);

private:
    struct pending_cancellation final {
        std::uint64_t id{};
        Http3ClientConnection* connection{};
        std::uint64_t request{};
    };
    struct migration_tracking final {
        std::uint64_t id{};
        std::size_t slot{};
        std::uint64_t generation{};
        std::uint64_t connection_migration_id{};
        ruvia::quic_path_migration result{};
    };
    asio::io_context& io_;
    const WorkerHandle& worker_;
    TaskScope& tasks_;
    const HttpClientConfigStorage& config_;
    HttpClientAdvertisementQueue& advertisements_;
    Http3ClientPushObserver push_;
    std::pmr::memory_resource* resource_;
    std::unique_ptr<http3_quic_client_tls_context, PmrObjectDeleter<http3_quic_client_tls_context>> tls_;
    std::unique_ptr<Http3ClientBodyBudget, PmrObjectDeleter<Http3ClientBodyBudget>> body_budget_;
    std::pmr::vector<std::unique_ptr<Http3ClientConnection, PmrObjectDeleter<Http3ClientConnection>>> connections_;
    WorkerSignal generation_signal_;
    std::pmr::vector<pending_cancellation> cancellations_;
    std::optional<migration_tracking> migration_;
    std::uint64_t next_migration_id_{1};
    bool retired_{};
};

}  // namespace ruvia::detail
