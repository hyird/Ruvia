#include <mysql.h>

#include <exception>
#include <memory_resource>
#include <utility>

#include "ruvia/web/detail/db/db_utils.h"

#include "db/db_registry.h"
#include "db/db_slot_socket.h"

namespace ruvia {
namespace {

[[nodiscard]] const worker_handle& require_mariadb_worker(const worker_handle& worker_value) {
    if (!worker_value.valid()) {
        throw std::invalid_argument("MariaDB pool requires a valid worker");
    }
    return worker_value;
}

}  // namespace

detail::mariadb_pool::connection_slot_type::connection_slot_type(
    asio::io_context& io_context, std::pmr::memory_resource* resource)
    : resolver_(io_context),
      wait_socket_(nullptr, slot_socket_deleter_type{detail::pmr_resource_or_default(resource)}),
      socket_quarantine_(detail::make_pmr_object<detail::db_slot_socket_quarantine>(
          detail::process_resource(), io_context)),
      deadline_timer_(detail::make_pmr_object<::ruvia::worker_timer_registration>(
          detail::pmr_resource_or_default(resource))) {}

detail::mariadb_pool::connection_slot_type::~connection_slot_type() {
    if (wait_active_) {
        std::terminate();
    }
    if (wait_socket_ != nullptr) {
        socket_quarantine_->retain(std::move(*wait_socket_), connection_);
        wait_socket_.reset();
        connection_ = nullptr;
        // Deliberately abandon the process-backed node: its socket destructor
        // and mysql_close() must not both close the same native handle.
        (void)socket_quarantine_.release();
    }
}
detail::mariadb_pool::connection_slot_type::connection_slot_type(connection_slot_type&&) noexcept = default;
detail::mariadb_pool::connection_slot_type& detail::mariadb_pool::connection_slot_type::operator=(
    connection_slot_type&&) noexcept = default;

detail::mariadb_pool::mariadb_pool(asio::io_context& io_context, const worker_handle& worker_value,
    db_config_storage config, std::pmr::memory_resource* resource)
    : io_context_(io_context),
      config_(std::move(config)),
      resource_(detail::pmr_resource_or_default(resource)),
      worker_(require_mariadb_worker(worker_value)),
      slots_(resource_),
      scheduler_(1, worker_, resource_),
      cancellation_target_(make_worker_cancellation_target(*this, worker_)) {
    if (config_.driver_ != db_driver::mariadb) {
        throw std::invalid_argument("MariaDB pool requires the MariaDB driver");
    }
    slots_.reserve(1);
    slots_.emplace_back(io_context_, resource_);
}

detail::mariadb_pool::~mariadb_pool() {
    cancellation_target_->detach(*this);
    close_now();
}

void detail::mariadb_pool::close_slot(connection_slot_type& slot) noexcept {
    slot.close_requested_ = true;
    slot.resolver_.cancel();
    if (slot.wait_active_) {
        const auto* active_kind = slot.deadline_.kind();
        if (active_kind != nullptr && *active_kind == connection_slot_type::deadline_kind_type::sleep) {
            auto handle = std::exchange(slot.deadline_continuation_, {});
            if (handle) {
                handle.resume();
            }
        } else if (slot.wait_socket_ != nullptr) {
            // Keep the wrapper and driver connection alive until every queued
            // wait completion has run. Cancellation wakes the coroutine; it
            // releases the borrowed socket before completing slot teardown.
            slot.wait_socket_->cancel();
        }
        return;
    }

    const auto* kind = slot.deadline_.kind();
    if (kind != nullptr && *kind == connection_slot_type::deadline_kind_type::socket &&
        slot.wait_socket_ != nullptr) {
        slot.wait_socket_->cancel();
    } else if (kind != nullptr && *kind == connection_slot_type::deadline_kind_type::sleep) {
        auto handle = std::exchange(slot.deadline_continuation_, {});
        if (handle) {
            handle.resume();
        }
    }
    detail::clear_db_slot_deadline(slot);
    if (slot.wait_socket_ != nullptr) {
        // release() leaves the wrapper attached on failure. Keep both objects
        // in the slot so a later close attempt can retry without either owner
        // closing the native socket behind the other.
        if (slot.wait_socket_->release()) {
            (void)scheduler_.close();
            return;
        }
        slot.wait_socket_.reset();
    }
    if (slot.connection_ != nullptr) {
        mysql_close(slot.connection_);
        slot.connection_ = nullptr;
    }
    slot.connected_ = false;
    slot.close_requested_ = false;
}

void detail::mariadb_pool::connection_slot_type::expire_deadline(
    connection_slot_type& slot, deadline_kind_type kind) noexcept {
    switch (kind) {
        case deadline_kind_type::resolve:
            slot.resolver_.cancel();
            break;
        case deadline_kind_type::socket:
            if (slot.wait_socket_ != nullptr) {
                slot.wait_socket_->cancel();
            }
            break;
        case deadline_kind_type::sleep: {
            auto handle = std::exchange(slot.deadline_continuation_, {});
            if (handle) {
                handle.resume();
            }
            break;
        }
    }
}

}  // namespace ruvia
