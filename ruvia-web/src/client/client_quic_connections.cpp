#include "client/client_quic_connections.h"

#include <algorithm>
#include <chrono>
#include <ranges>

#include "client/http_client_advertisement_queue.h"
#include "http3/http3_quic_client_tls_context.h"

namespace ruvia::detail {

client_quic_connections::client_quic_connections(asio::io_context& io, const worker_handle& worker_value,
    task_scope& tasks, const http_client_config_storage& config, http_client_advertisement_queue& advertisements,
    http3_client_push_observer push, std::pmr::memory_resource* resource)
    : io_(io),
      worker_(worker_value),
      tasks_(tasks),
      config_(config),
      advertisements_(advertisements),
      push_(push),
      resource_(resource),
      tls_(nullptr, pmr_object_deleter<http3_quic_client_tls_context>{resource}),
      body_budget_(nullptr, pmr_object_deleter<http3_client_body_budget>{resource}),
      connections_(resource),
      generation_signal_(worker_value),
      cancellations_(resource) {
    if (config.protocol_ != http_client_protocol::http3_only) {
        return;
    }
    tls_ = make_pmr_object<http3_quic_client_tls_context>(resource, config.transport_.view(), resource);
    body_budget_ = make_pmr_object<http3_client_body_budget>(resource, std::size_t{64} * 1024 * 1024);
    connections_.reserve(config.connection_count_);
    for (std::size_t i = 0; i < config.connection_count_; ++i) {
        connections_.emplace_back(nullptr, pmr_object_deleter<http3_client_connection>{resource});
    }
}
client_quic_connections::~client_quic_connections() = default;
void client_quic_connections::request_stop() noexcept {
    for (auto& connection : connections_) {
        if (connection) {
            connection->request_stop();
        }
    }
}
void client_quic_connections::register_cancellation(std::uint64_t id, http3_client_connection* connection, std::uint64_t request) {
    cancellations_.push_back({id, connection, request});
}
void client_quic_connections::unregister_cancellation(std::uint64_t id) noexcept {
    std::erase_if(cancellations_, [id](const pending_cancellation& pending) { return pending.id_ == id; });
}
bool client_quic_connections::cancel(std::uint64_t id) noexcept {
    const auto found = std::ranges::find_if(cancellations_, [id](const pending_cancellation& pending) { return pending.id_ == id; });
    if (found == cancellations_.end()) {
        return false;
    }
    if (found->connection_ == nullptr) {
        generation_signal_.notify();
    } else {
        found->connection_->cancel(found->request_);
    }
    return true;
}
ruvia::quic_path_migration client_quic_connections::start_path_migration(
    const asio::ip::udp::endpoint& local_endpoint) {
    if (!worker_.is_current() || retired_) {
        return {.status_ = ruvia::quic_migration_status::rejected};
    }
    if (migration_ && migration_->result_.status_ == ruvia::quic_migration_status::started) {
        const auto& active = migration_;
        if (active->slot_ < connections_.size()) {
            const auto& connection = connections_[active->slot_];
            if (connection) {
                const auto status = connection->path_migration(active->connection_migration_id_);
                if (status) {
                    migration_->result_ = *status;
                    migration_->result_.id_ = active->id_;
                    if (status->status_ == ruvia::quic_migration_status::started) {
                        return {.status_ = ruvia::quic_migration_status::rejected};
                    }
                }
            }
        }
        if (migration_->result_.status_ == ruvia::quic_migration_status::started) {
            migration_->result_.status_ = ruvia::quic_migration_status::aborted;
        }
    }
    for (std::size_t slot = 0; slot < connections_.size(); ++slot) {
        const auto& connection = connections_[slot];
        if (!connection || !connection->running()) {
            continue;
        }
        const auto result_value = connection->start_path_migration(local_endpoint);
        if (result_value.status_ == ruvia::quic_migration_status::started ||
            result_value.status_ == ruvia::quic_migration_status::validated) {
            auto id = next_migration_id_++;
            if (id == 0) {
                id = next_migration_id_++;
            }
            migration_ = migration_tracking{
                id, slot, connection->quic_generation(), result_value.id_, result_value};
            migration_->result_.id_ = id;
            return migration_->result_;
        }
        if (result_value.status_ == ruvia::quic_migration_status::would_block) {
            return result_value;
        }
    }
    return {.status_ = ruvia::quic_migration_status::rejected};
}

std::optional<ruvia::quic_path_migration> client_quic_connections::path_migration(
    std::uint64_t id) const noexcept {
    if (!worker_.is_current() || !migration_ || migration_->id_ != id) {
        return std::nullopt;
    }
    const auto& migration = *migration_;
    const auto fallback = [&migration, id] {
        auto result_value = migration.result_;
        result_value.id_ = id;
        if (result_value.status_ == ruvia::quic_migration_status::started) {
            result_value.status_ = ruvia::quic_migration_status::aborted;
        }
        return result_value;
    };
    if (migration.slot_ >= connections_.size()) {
        return fallback();
    }
    const auto& connection = connections_[migration.slot_];
    if (!connection) {
        return fallback();
    }
    const auto status = connection->path_migration(migration.connection_migration_id_);
    if (!status) {
        return fallback();
    }
    auto result_value = *status;
    result_value.id_ = id;
    return result_value;
}

ruvia::quic_operation_status client_quic_connections::cancel_path_migration(std::uint64_t id) {
    if (!worker_.is_current() || !migration_ || migration_->id_ != id ||
        migration_->slot_ >= connections_.size()) {
        return ruvia::quic_operation_status::retired;
    }
    const auto& migration = *migration_;
    const auto& connection = connections_[migration.slot_];
    if (!connection || connection->quic_generation() != migration.generation_) {
        return ruvia::quic_operation_status::retired;
    }
    return connection->cancel_path_migration(migration.connection_migration_id_);
}

http3_client_connection& client_quic_connections::acquire(std::size_t connection_index) {
    if (connections_.empty() || tls_ == nullptr || body_budget_ == nullptr) {
        throw http_client_error(
            http_client_error::code_type::protocol_unavailable, "HTTP/3 client is not configured");
    }
    auto& owner_value = connections_.at(connection_index % connections_.size());
    if (!owner_value || (owner_value->terminal() && !owner_value->running() && owner_value->retained_requests() == 0)) {
        const auto origin = http_origin_view::https(
            {.host_ = config_.host_, .port_ = config_.port_});
        auto push = push_;
        push.connection_slot_ = connection_index % connections_.size();
        owner_value = make_pmr_object<http3_client_connection>(resource_, io_, worker_,
            tasks_, *tls_, origin, config_.connect_timeout_, resource_,
            requests_per_connection, config_.max_response_bytes_,
            std::chrono::seconds(30), body_budget_.get(), config_.write_timeout_,
            http3_client_connection::lifecycle_notification_type{
                .context_ = &generation_signal_,
                .notify_ = [](void* context_value) noexcept {
                    static_cast<worker_signal*>(context_value)->notify();
                },
            },
            config_.http3_qpack_, config_.advertisements_.receive_origins_ ? http3_client_origin_observer{.context_ = &advertisements_, .connection_slot_ = connection_index % connections_.size(), .receive_ = [](void* raw, std::size_t slot, const http_origin_advertisement& origins) {
                                                                                                              (void)static_cast<http_client_advertisement_queue*>(raw)->retain(slot, http_protocol_version::http3, origins);
                                                                                                          }}
                                                                           : http3_client_origin_observer{},
            push, config_.initial_quic_version_, config_.http3_early_data_);
    }
    return *owner_value;
}

void client_quic_connections::retire() noexcept {
    retired_ = true;
    if (migration_ && migration_->slot_ < connections_.size()) {
        const auto& connection = connections_[migration_->slot_];
        if (connection) {
            if (const auto migration =
                    connection->path_migration(migration_->connection_migration_id_)) {
                migration_->result_ = *migration;
                migration_->result_.id_ = migration_->id_;
            }
        }
        if (migration_->result_.status_ == ruvia::quic_migration_status::started) {
            migration_->result_.status_ = ruvia::quic_migration_status::aborted;
        }
    }
    for (auto& connection : connections_) {
        connection.reset();
    }
}

}  // namespace ruvia::detail
