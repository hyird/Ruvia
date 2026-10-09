#include <utility>

#include "ruvia/web/db/db.h"
#include "ruvia/web/detail/db/db_utils.h"

#include "db/db_registry.h"

// A streaming query result: the rows arrive one at a time from a pooled
// connection the result holds for as long as it is active, and every exit --
// exhaustion, explicit close, failure, or destruction -- must return that
// connection exactly once.

namespace ruvia {
namespace {

task<std::optional<db_row>> read_pool_stream(detail::db_pool_ref_type pool, std::size_t slot, void* result_value,
    std::pmr::memory_resource* resource, const operation_options& options) {
    return detail::visit_db_pool(
        pool, [&](auto& client) { return client.read_stream_row(slot, result_value, resource, options); });
}

task<void> close_pool_stream(detail::db_pool_ref_type pool, std::size_t slot, void* result_value,
    std::pmr::memory_resource* resource, const operation_options& options) {
    return detail::visit_db_pool(
        pool, [&](auto& client) { return client.close_stream(slot, result_value, resource, options); });
}

void abort_pool_stream(detail::db_pool_ref_type pool, std::size_t slot, void* result_value) noexcept {
    detail::visit_db_pool_if_present(
        pool, [&](auto& client) noexcept { client.abort_stream(slot, result_value); });
}

}  // namespace

db_stream_result::lease_type::lease_type(detail::db_pool_ref_type client, std::size_t slot, void* result_value,
    std::pmr::memory_resource* resource, operation_options options) noexcept
    : client_(client),
      slot_(slot),
      result_(result_value),
      resource_(detail::pmr_resource_or_default(resource)),
      options_(std::move(options)) {}

class db_stream_result::state_type final {
public:
    state_type(detail::db_pool_ref_type client, std::size_t slot, void* result_value,
        std::pmr::memory_resource* resource, operation_options options) noexcept
        : operation_(lease_type{client, slot, result_value, resource, std::move(options)}) {}

    ~state_type() {
        operation_.reset(
            [](lease_type& lease_value) noexcept { abort_pool_stream(lease_value.client_, lease_value.slot_, lease_value.result_); });
    }

    operation_state_type operation_;
};

db_stream_result::db_stream_result(detail::db_pool_ref_type client, std::size_t slot, void* result_value,
    std::pmr::memory_resource* resource, operation_options options)
    : state_(detail::make_pmr_object<state_type>(
          resource, client, slot, result_value, resource, std::move(options))) {}

db_stream_result::db_stream_result(db_stream_result&& other) noexcept
    : state_(std::move(other.state_)),
      registration_(std::move(other.registration_), this) {}

db_stream_result::~db_stream_result() = default;

bool db_stream_result::active() const noexcept {
    return state_ != nullptr && state_->operation_.active();
}

void db_stream_result::bind_operation_scope(::ruvia::operation_scope& scope) noexcept {
    registration_.bind(scope, this, &db_stream_result::expire_capability);
}

void db_stream_result::expire_capability(void* target) noexcept {
    auto& result_value = *static_cast<db_stream_result*>(target);
    result_value.reset();
}

scoped_operation<std::optional<db_row>> db_stream_result::read() & {
    registration_.require_active();
    return ::ruvia::make_scoped_operation(
        registration_.scope(), read_task(operation_guard_type(state_->operation_)));
}

task<std::optional<db_row>> db_stream_result::read_task(operation_guard_type pending) {
    operation_guard_type operation(std::move(pending));
    operation.start();
    auto& lease_value = operation.lease();
    auto row = co_await read_pool_stream(
        lease_value.client_, lease_value.slot_, lease_value.result_, lease_value.resource_, lease_value.options_);
    if (row) {
        operation.finish_active();
    } else {
        operation.finish_closed();
    }
    co_return row;
}

scoped_operation<void> db_stream_result::close() & {
    registration_.require_active();
    return ::ruvia::make_scoped_operation(
        registration_.scope(), close_task(operation_guard_type(state_->operation_)));
}

task<void> db_stream_result::close_task(operation_guard_type pending) {
    operation_guard_type operation(std::move(pending));
    operation.start();
    auto& lease_value = operation.lease();
    co_await close_pool_stream(lease_value.client_, lease_value.slot_, lease_value.result_, lease_value.resource_, lease_value.options_);
    operation.finish_closed();
}

void db_stream_result::reset() noexcept {
    if (state_ != nullptr) {
        state_->operation_.reset(
            [](lease_type& lease_value) noexcept { abort_pool_stream(lease_value.client_, lease_value.slot_, lease_value.result_); });
    }
}

}  // namespace ruvia
