#include "client/client_quic_connections.h"

#include <algorithm>
#include <chrono>
#include <ranges>

#include "client/HttpClientAdvertisementQueue.h"
#include "http3/Http3QuicClientTlsContext.h"

namespace ruvia::detail {

client_quic_connections::client_quic_connections(asio::io_context& io, const WorkerHandle& worker,
    TaskScope& tasks, const HttpClientConfigStorage& config, HttpClientAdvertisementQueue& advertisements,
    Http3ClientPushObserver push, std::pmr::memory_resource* resource)
    : io_(io),
      worker_(worker),
      tasks_(tasks),
      config_(config),
      advertisements_(advertisements),
      push_(push),
      resource_(resource),
      tls_(nullptr, PmrObjectDeleter<http3_quic_client_tls_context>{resource}),
      body_budget_(nullptr, PmrObjectDeleter<Http3ClientBodyBudget>{resource}),
      connections_(resource),
      generation_signal_(worker),
      cancellations_(resource) {
    if (config.protocol != HttpClientProtocol::kHttp3Only) {
        return;
    }
    tls_ = makePmrObject<http3_quic_client_tls_context>(resource, config.transport.view(), resource);
    body_budget_ = makePmrObject<Http3ClientBodyBudget>(resource, std::size_t{64} * 1024 * 1024);
    connections_.reserve(config.connectionCount);
    for (std::size_t i = 0; i < config.connectionCount; ++i) {
        connections_.emplace_back(nullptr, PmrObjectDeleter<Http3ClientConnection>{resource});
    }
}
client_quic_connections::~client_quic_connections() = default;
void client_quic_connections::request_stop() noexcept {
    for (auto& connection : connections_) {
        if (connection) {
            connection->requestStop();
        }
    }
}
void client_quic_connections::register_cancellation(std::uint64_t id, Http3ClientConnection* connection, std::uint64_t request) {
    cancellations_.push_back({id, connection, request});
}
void client_quic_connections::unregister_cancellation(std::uint64_t id) noexcept {
    std::erase_if(cancellations_, [id](const pending_cancellation& pending) { return pending.id == id; });
}
bool client_quic_connections::cancel(std::uint64_t id) noexcept {
    const auto found = std::ranges::find_if(cancellations_, [id](const pending_cancellation& pending) { return pending.id == id; });
    if (found == cancellations_.end()) {
        return false;
    }
    if (found->connection == nullptr) {
        generation_signal_.notify();
    } else {
        found->connection->cancel(found->request);
    }
    return true;
}
ruvia::quic_path_migration client_quic_connections::start_path_migration(
    const asio::ip::udp::endpoint& local_endpoint) {
    if (!worker_.isCurrent() || retired_) {
        return {.status = ruvia::quic_migration_status::rejected};
    }
    if (migration_ && migration_->result.status == ruvia::quic_migration_status::started) {
        const auto& active = migration_;
        if (active->slot < connections_.size()) {
            const auto& connection = connections_[active->slot];
            if (connection) {
                const auto status = connection->path_migration(active->connection_migration_id);
                if (status) {
                    migration_->result = *status;
                    migration_->result.id = active->id;
                    if (status->status == ruvia::quic_migration_status::started) {
                        return {.status = ruvia::quic_migration_status::rejected};
                    }
                }
            }
        }
        if (migration_->result.status == ruvia::quic_migration_status::started) {
            migration_->result.status = ruvia::quic_migration_status::aborted;
        }
    }
    for (std::size_t slot = 0; slot < connections_.size(); ++slot) {
        const auto& connection = connections_[slot];
        if (!connection || !connection->running()) {
            continue;
        }
        const auto result = connection->start_path_migration(local_endpoint);
        if (result.status == ruvia::quic_migration_status::started ||
            result.status == ruvia::quic_migration_status::validated) {
            auto id = next_migration_id_++;
            if (id == 0) {
                id = next_migration_id_++;
            }
            migration_ = migration_tracking{
                id, slot, connection->quic_generation(), result.id, result};
            migration_->result.id = id;
            return migration_->result;
        }
        if (result.status == ruvia::quic_migration_status::would_block) {
            return result;
        }
    }
    return {.status = ruvia::quic_migration_status::rejected};
}

std::optional<ruvia::quic_path_migration> client_quic_connections::path_migration(
    std::uint64_t id) const noexcept {
    if (!worker_.isCurrent() || !migration_ || migration_->id != id) {
        return std::nullopt;
    }
    const auto& migration = *migration_;
    const auto fallback = [&migration, id] {
        auto result = migration.result;
        result.id = id;
        if (result.status == ruvia::quic_migration_status::started) {
            result.status = ruvia::quic_migration_status::aborted;
        }
        return result;
    };
    if (migration.slot >= connections_.size()) {
        return fallback();
    }
    const auto& connection = connections_[migration.slot];
    if (!connection) {
        return fallback();
    }
    const auto status = connection->path_migration(migration.connection_migration_id);
    if (!status) {
        return fallback();
    }
    auto result = *status;
    result.id = id;
    return result;
}

ruvia::quic_operation_status client_quic_connections::cancel_path_migration(std::uint64_t id) {
    if (!worker_.isCurrent() || !migration_ || migration_->id != id ||
        migration_->slot >= connections_.size()) {
        return ruvia::quic_operation_status::retired;
    }
    const auto& migration = *migration_;
    const auto& connection = connections_[migration.slot];
    if (!connection || connection->quic_generation() != migration.generation) {
        return ruvia::quic_operation_status::retired;
    }
    return connection->cancel_path_migration(migration.connection_migration_id);
}

Http3ClientConnection& client_quic_connections::acquire(std::size_t connectionIndex) {
    if (connections_.empty() || tls_ == nullptr || body_budget_ == nullptr) {
        throw HttpClientError(
            HttpClientError::Code::kProtocolUnavailable, "HTTP/3 client is not configured");
    }
    auto& owner = connections_.at(connectionIndex % connections_.size());
    if (!owner || (owner->terminal() && !owner->running() && owner->retainedRequests() == 0)) {
        const auto origin = HttpOriginView::https(
            {.host = config_.host, .port = config_.port});
        auto push = push_;
        push.connectionSlot = connectionIndex % connections_.size();
        owner = makePmrObject<Http3ClientConnection>(resource_, io_, worker_,
            tasks_, *tls_, origin, config_.connectTimeout, resource_,
            requests_per_connection, config_.maxResponseBytes,
            std::chrono::seconds(30), body_budget_.get(), config_.write_timeout,
            Http3ClientConnection::LifecycleNotification{
                .context = &generation_signal_,
                .notify = [](void* context) noexcept {
                    static_cast<WorkerSignal*>(context)->notify();
                },
            },
            config_.http3Qpack, config_.advertisements.receiveOrigins ? Http3ClientOriginObserver{.context = &advertisements_, .connectionSlot = connectionIndex % connections_.size(), .receive = [](void* raw, std::size_t slot, const HttpOriginAdvertisement& origins) {
                                                                                                      (void)static_cast<HttpClientAdvertisementQueue*>(raw)->retain(slot, HttpProtocolVersion::kHttp3, origins);
                                                                                                  }}
                                                                      : Http3ClientOriginObserver{},
            push, config_.initial_quic_version, config_.http3_early_data);
    }
    return *owner;
}

void client_quic_connections::retire() noexcept {
    retired_ = true;
    if (migration_ && migration_->slot < connections_.size()) {
        const auto& connection = connections_[migration_->slot];
        if (connection) {
            if (const auto migration =
                    connection->path_migration(migration_->connection_migration_id)) {
                migration_->result = *migration;
                migration_->result.id = migration_->id;
            }
        }
        if (migration_->result.status == ruvia::quic_migration_status::started) {
            migration_->result.status = ruvia::quic_migration_status::aborted;
        }
    }
    for (auto& connection : connections_) {
        connection.reset();
    }
}

}  // namespace ruvia::detail
