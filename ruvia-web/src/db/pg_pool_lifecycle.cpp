#include <libpq-fe.h>

#include <exception>
#include <stdexcept>
#include <utility>

#include "ruvia/web/detail/db/db_utils.h"

#include "db/db_registry.h"
#include "db/db_slot_socket.h"

namespace ruvia::detail {
namespace {

[[nodiscard]] const worker_handle& require_postgresql_worker(const worker_handle& worker_value) {
    if (!worker_value.valid()) {
        throw std::invalid_argument("PostgreSQL pool requires a valid worker");
    }
    return worker_value;
}

}  // namespace

postgresql_pool::connection_slot_type::connection_slot_type(
    asio::io_context& io_context, std::pmr::memory_resource* resource)
    : resolver_(io_context),
      wait_socket_(nullptr, slot_socket_deleter_type{pmr_resource_or_default(resource)}),
      socket_quarantine_(make_pmr_object<db_slot_socket_quarantine>(process_resource(), io_context)),
      deadline_timer_(make_pmr_object<worker_timer_registration>(pmr_resource_or_default(resource))) {}

postgresql_pool::connection_slot_type::~connection_slot_type() {
    if (wait_active_) {
        std::terminate();
    }
    if (wait_socket_ != nullptr) {
        socket_quarantine_->retain(std::move(*wait_socket_), connection_);
        wait_socket_.reset();
        connection_ = nullptr;
        // Deliberately abandon the process-backed node: its socket destructor
        // and PQfinish() must not both close the same native handle.
        (void)socket_quarantine_.release();
    }
}
postgresql_pool::connection_slot_type::connection_slot_type(connection_slot_type&&) noexcept = default;
postgresql_pool::connection_slot_type& postgresql_pool::connection_slot_type::operator=(
    connection_slot_type&&) noexcept = default;

postgresql_pool::postgresql_pool(asio::io_context& io_context, const worker_handle& worker_value,
    db_config_storage config, std::pmr::memory_resource* resource)
    : io_context_(io_context),
      config_(std::move(config)),
      resource_(pmr_resource_or_default(resource)),
      worker_(require_postgresql_worker(worker_value)),
      slots_(resource_),
      scheduler_(1, worker_, resource_),
      cancellation_target_(make_worker_cancellation_target(*this, worker_)) {
    if (config_.driver_ != db_driver::postgresql) {
        throw std::invalid_argument("PostgreSQL pool requires the PostgreSQL driver");
    }
    slots_.reserve(1);
    slots_.emplace_back(io_context_, resource_);
}

postgresql_pool::~postgresql_pool() {
    cancellation_target_->detach(*this);
    close_now();
}

void postgresql_pool::close_slot(connection_slot_type& slot) noexcept {
    slot.close_requested_ = true;
    slot.resolver_.cancel();
    if (slot.wait_active_) {
        if (slot.wait_socket_ != nullptr) {
            // The wait callback lives in the suspended task frame and still
            // refers to this wrapper. Cancel it now; final disposal follows
            // after the callback drains and releases the borrowed socket.
            slot.wait_socket_->cancel();
        }
        return;
    }

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
    clear_db_slot_deadline(slot);
    if (slot.connection_ != nullptr) {
        PQfinish(slot.connection_);
        slot.connection_ = nullptr;
    }
    slot.connected_ = false;
    slot.close_requested_ = false;
}

void postgresql_pool::connection_slot_type::expire_deadline(
    connection_slot_type& slot, deadline_kind_type kind) noexcept {
    if (kind == deadline_kind_type::resolve) {
        slot.resolver_.cancel();
    } else if (slot.wait_socket_ != nullptr) {
        slot.wait_socket_->cancel();
    }
}

}  // namespace ruvia::detail
