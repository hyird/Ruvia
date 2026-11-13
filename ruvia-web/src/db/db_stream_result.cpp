#include <utility>

#include "ruvia/web/db/db.h"

#include "db/db_pool.h"
#include "integration/request_io_metrics.h"

// A streaming query result: the rows arrive one at a time from a pooled
// connection the result holds for as long as it is active, and every exit --
// exhaustion, explicit close, failure, or destruction -- must return that
// connection exactly once.

namespace ruvia {
db_stream_result::lease_type::lease_type(detail::db_pool_ref_type client, std::size_t slot,
    std::pmr::memory_resource* resource, operation_options options) noexcept
    : client_(client),
      slot_(slot),
      resource_(pmr_resource_or_default(resource)),
      fields_(resource_),
      options_(std::move(options)) {}

void db_stream_result::lease_type::abort() noexcept {
    detail::visit_db_pool_if_present(client_, [&](auto& client) noexcept { client.abort_stream(slot_); });
}

db_stream_result::state_type::state_type(
    lease_type lease, std::pmr::vector<std::pmr::string> column_names) noexcept
    : column_names_(std::move(column_names)),
      operation_(std::move(lease)) {}

db_stream_result::db_stream_result(detail::db_pool_ref_type client, std::size_t slot,
    std::pmr::vector<std::pmr::string> column_names, std::pmr::memory_resource* resource,
    operation_options options)
    : state_(make_pmr_object<state_type>(
          resource, lease_type{client, slot, resource, std::move(options)}, std::move(column_names))) {}

db_stream_result::db_stream_result(db_stream_result&& other) noexcept
    : state_(std::move(other.state_)),
      io_metrics_(other.io_metrics_),
      registration_(std::move(other.registration_), this) {}

db_stream_result::~db_stream_result() = default;

bool db_stream_result::active() const noexcept {
    return state_ != nullptr && state_->operation_.active();
}

std::span<const std::pmr::string> db_stream_result::column_names() const& {
    registration_.require_active();
    return state_ != nullptr ? std::span<const std::pmr::string>(state_->column_names_)
                             : std::span<const std::pmr::string>{};
}

void db_stream_result::bind_operation_scope(
    ::ruvia::operation_scope& scope, detail::request_io_metrics* io_metrics) noexcept {
    io_metrics_ = io_metrics;
    registration_.bind(scope, this, &db_stream_result::expire_capability);
}

void db_stream_result::expire_capability(void* target) noexcept {
    auto& result_value = *static_cast<db_stream_result*>(target);
    result_value.reset();
}

namespace {

// The stream's handle options with this operation's own: its timeout only
// tightens, a supplied token replaces the inherited one. An already-stopped
// token gives the lease back before it reaches the connection, so the stream
// stays usable.
template <typename guard_type>
[[nodiscard]] operation_options admit_stream_operation(guard_type& operation, operation_options operation_value) {
    auto effective = override_operation_options(operation.lease().options_, std::move(operation_value));
    if (effective.stop_token_.stop_requested()) {
        operation.finish_active();
        throw db_error(db_error::code_type::cancelled, std::nullopt, "database stream operation cancelled");
    }
    return effective;
}

}  // namespace

task<std::optional<db_row>> db_stream_result::read(operation_options operation) & {
    registration_.require_active();
    return ::ruvia::make_scoped_operation(registration_.scope(),
        read_task(io_metrics_, operation_guard_type(state_->operation_), state_->column_names_, std::move(operation)));
}

task<std::optional<db_row>> db_stream_result::read_task(
    detail::request_io_metrics* io_metrics, operation_guard_type pending,
    std::pmr::vector<std::pmr::string>& column_names, operation_options operation_value) {
    detail::scoped_request_io_timer timer(io_metrics, detail::request_io_kind::db);
    operation_guard_type operation(std::move(pending));
    operation.start();
    auto effective = admit_stream_operation(operation, std::move(operation_value));
    auto& lease_value = operation.lease();
    auto row = co_await detail::visit_db_pool(lease_value.client_, [&](auto& client) {
        return client.read_stream_row(lease_value.slot_, lease_value.fields_, column_names, lease_value.resource_, effective);
    });
    if (row) {
        operation.finish_active();
    } else {
        operation.finish_closed();
    }
    co_return row;
}

task<void> db_stream_result::close(operation_options operation) & {
    registration_.require_active();
    return ::ruvia::make_scoped_operation(
        registration_.scope(), close_task(io_metrics_, operation_guard_type(state_->operation_), std::move(operation)));
}

task<void> db_stream_result::close_task(
    detail::request_io_metrics* io_metrics, operation_guard_type pending, operation_options operation_value) {
    detail::scoped_request_io_timer timer(io_metrics, detail::request_io_kind::db);
    operation_guard_type operation(std::move(pending));
    if (operation.finished()) {
        co_return;
    }
    operation.start();
    auto effective = admit_stream_operation(operation, std::move(operation_value));
    auto& lease_value = operation.lease();
    co_await detail::visit_db_pool(
        lease_value.client_, [&](auto& client) { return client.close_stream(lease_value.slot_, effective); });
    operation.finish_closed();
}

void db_stream_result::reset() noexcept {
    if (state_ != nullptr) {
        state_->operation_.reset();
    }
}

}  // namespace ruvia
