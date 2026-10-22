#pragma once

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <memory_resource>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "ruvia/core/event_loop.h"
#include "ruvia/core/memory/pmr_object.h"
#include "ruvia/core/scoped_operation.h"
#include "ruvia/core/task.h"
#include "ruvia/web/db/db_rows.h"
#include "ruvia/web/db/db_types.h"
#include "ruvia/web/detail/db/db_backend.h"
#include "ruvia/web/detail/db/db_mapped_query.h"
#include "ruvia/web/detail/db/db_parameter_pack.h"
#include "ruvia/web/detail/db/db_sql_literal.h"

namespace ruvia {

class db_transaction;
class db_query;
template <typename entity_type, typename executor_type>
class db_repository;
template <typename entity_type, typename executor_type>
class db_query_builder;
template <typename entity_type, typename executor_type>
class db_write_query_builder;
#ifdef RUVIA_ENABLE_REDIS
class redis_handle;
#endif

namespace detail {
class db_client_state;
class db_query_cache_state;
struct db_query_backend;
struct db_query_plan;
}  // namespace detail

// Statement and completion operations are lazy. Inputs are owned before the
// entry returns, so cold operations may be prepared while a statement runs.
// Only starting overlapping operations throws std::logic_error; that rejected
// operation does not abort the statement already holding the transaction.
class db_transaction final {
public:
    db_transaction(const db_transaction&) = delete;
    db_transaction& operator=(const db_transaction&) = delete;
    // Operations borrow the address-stable state owned by this object, so a
    // move transfers the state without invalidating a cold or running frame.
    db_transaction(db_transaction&& other) noexcept;
    db_transaction& operator=(db_transaction&&) = delete;
    ~db_transaction();

    [[nodiscard]] bool active() const noexcept;

    template <typename entity_type>
    [[nodiscard]] db_repository<entity_type, db_transaction> repository() &;
    template <typename entity_type>
    db_repository<entity_type, db_transaction> repository() && = delete;

    scoped_operation<db_rows> query(std::string_view sql, std::span<const db_value> params = {}) &;
    scoped_operation<db_rows> query(std::string_view, std::span<const db_value> = {}) && = delete;
    scoped_operation<db_rows> query(std::string_view, std::initializer_list<db_value>) & = delete;
    scoped_operation<db_rows> query(std::string_view, std::initializer_list<db_value>) && = delete;
    scoped_operation<db_exec_result> execute(
        std::string_view sql, std::span<const db_value> params = {}) &;
    scoped_operation<db_exec_result> execute(
        std::string_view, std::span<const db_value> = {}) && = delete;
    scoped_operation<db_exec_result> execute(
        std::string_view, std::initializer_list<db_value>) & = delete;
    scoped_operation<db_exec_result> execute(
        std::string_view, std::initializer_list<db_value>) && = delete;

    // Bound parameters as ordinary arguments, with the same synchronous cloning
    // and the same temporary-safety as db_handle::query()/execute().
    // Fixed SQL uses MariaDB placeholders by default; specify postgresql
    // for numbered parameters. Values are owned by the existing operation path.
    template <fixed_string sql, db_driver driver = db_driver::mariadb, typename... params_type>
        requires(detail::db_parameter<params_type> && ...)
    [[nodiscard]] scoped_operation<db_rows> query(params_type&&... params) & {
        detail::validate_db_sql_literal<sql, driver, sizeof...(params_type)>(query_driver());
        return query(sql.view(), std::forward<params_type>(params)...);
    }
    template <fixed_string sql, db_driver driver = db_driver::mariadb, typename... params_type>
        requires(detail::db_parameter<params_type> && ...)
    scoped_operation<db_rows> query(params_type&&...) && = delete;

    template <fixed_string sql, db_driver driver = db_driver::mariadb, typename... params_type>
        requires(detail::db_parameter<params_type> && ...)
    [[nodiscard]] scoped_operation<db_exec_result> execute(params_type&&... params) & {
        detail::validate_db_sql_literal<sql, driver, sizeof...(params_type)>(query_driver());
        return execute(sql.view(), std::forward<params_type>(params)...);
    }
    template <fixed_string sql, db_driver driver = db_driver::mariadb, typename... params_type>
        requires(detail::db_parameter<params_type> && ...)
    scoped_operation<db_exec_result> execute(params_type&&...) && = delete;

    template <typename... params_type>
        requires detail::db_parameter_pack<params_type...>
    [[nodiscard]] scoped_operation<db_rows> query(std::string_view sql, params_type&&... params) & {
        const db_value values[]{detail::make_immediate_db_parameter(std::forward<params_type>(params))...};
        return query(sql, std::span<const db_value>(values));
    }
    template <typename... params_type>
        requires detail::db_parameter_pack<params_type...>
    scoped_operation<db_rows> query(std::string_view, params_type&&...) && = delete;

    template <typename... params_type>
        requires detail::db_parameter_pack<params_type...>
    [[nodiscard]] scoped_operation<db_exec_result> execute(
        std::string_view sql, params_type&&... params) & {
        const db_value values[]{detail::make_immediate_db_parameter(std::forward<params_type>(params))...};
        return execute(sql, std::span<const db_value>(values));
    }
    template <typename... params_type>
        requires detail::db_parameter_pack<params_type...>
    scoped_operation<db_exec_result> execute(std::string_view, params_type&&...) && = delete;

    scoped_operation<void> commit() &;
    scoped_operation<void> commit() && = delete;
    scoped_operation<void> rollback() &;
    scoped_operation<void> rollback() && = delete;
    scoped_operation<void> savepoint(std::string_view name) &;
    scoped_operation<void> savepoint(std::string_view) && = delete;
    scoped_operation<void> rollback_to_savepoint(std::string_view name) &;
    scoped_operation<void> rollback_to_savepoint(std::string_view) && = delete;
    scoped_operation<void> release_savepoint(std::string_view name) &;
    scoped_operation<void> release_savepoint(std::string_view) && = delete;
    scoped_operation<void> notify(std::string_view channel, std::string_view payload = {}) &;
    scoped_operation<void> notify(std::string_view, std::string_view = {}) && = delete;
    scoped_operation<void> advisory_lock(std::int64_t key) &;
    scoped_operation<void> advisory_lock(std::int64_t) && = delete;
    scoped_operation<void> advisory_lock(std::int32_t class_id, std::int32_t object_id) &;
    scoped_operation<void> advisory_lock(std::int32_t, std::int32_t) && = delete;
    scoped_operation<void> advisory_lock(std::string_view key, std::int64_t seed = 0) &;
    scoped_operation<void> advisory_lock(std::string_view, std::int64_t = 0) && = delete;
    [[nodiscard]] scoped_operation<bool> try_advisory_lock(std::int64_t key) &;
    scoped_operation<bool> try_advisory_lock(std::int64_t) && = delete;
    [[nodiscard]] scoped_operation<bool> try_advisory_lock(std::int32_t class_id, std::int32_t object_id) &;
    scoped_operation<bool> try_advisory_lock(std::int32_t, std::int32_t) && = delete;
    [[nodiscard]] scoped_operation<bool> try_advisory_lock(std::string_view key, std::int64_t seed = 0) &;
    scoped_operation<bool> try_advisory_lock(std::string_view, std::int64_t = 0) && = delete;
    scoped_operation<void> declare_cursor(
        std::string_view name, std::string_view sql, std::span<const db_value> params = {}, bool scroll = false) &;
    scoped_operation<void> declare_cursor(
        std::string_view, std::string_view, std::span<const db_value> = {}, bool = false) && = delete;
    scoped_operation<void> declare_cursor(
        std::string_view, std::string_view, std::initializer_list<db_value>, bool = false) & = delete;
    scoped_operation<void> declare_cursor(
        std::string_view, std::string_view, std::initializer_list<db_value>, bool = false) && = delete;
    [[nodiscard]] scoped_operation<db_rows> fetch_cursor(std::string_view name, std::uint64_t count = 1) &;
    scoped_operation<db_rows> fetch_cursor(std::string_view, std::uint64_t = 1) && = delete;
    scoped_operation<void> close_cursor(std::string_view name) &;
    scoped_operation<void> close_cursor(std::string_view) && = delete;

private:
    friend class db_handle;
    template <typename, typename>
    friend class db_repository;
    template <typename, typename>
    friend class db_query_builder;
    template <typename, typename>
    friend class db_write_query_builder;

    [[nodiscard]] db_driver query_driver() const;
    [[nodiscard]] std::pmr::memory_resource* query_resource() const;
    [[nodiscard]] scoped_operation<db_exec_result> execute_query(const db_query& query);
    [[nodiscard]] task<db_rows> query_task(const db_query& query);
    [[nodiscard]] task<std::pair<db_rows, db_rows>> query_and_count_task(const db_query& query, const db_query& count);
    template <typename result_type, typename mapper_type>
    [[nodiscard]] scoped_operation<std::pair<result_type, std::uint64_t>> query_mapped_and_count(const db_query& query, const db_query& count, mapper_type mapper) {
        registration_.require_active();
        auto& scope = registration_.scope();
        auto* resource = query_resource();
        auto task_value = query_and_count_task(query, count);
        return make_scoped_operation(scope,
            detail::map_db_query_and_count<result_type>(scope, std::move(task_value), resource, std::move(mapper)));
    }
    template <typename result_type, typename mapper_type>
    [[nodiscard]] scoped_operation<result_type> query_mapped(const db_query& query, mapper_type mapper) {
        registration_.require_active();
        auto& scope = registration_.scope();
        auto* resource = query_resource();
        auto task_value = query_task(query);
        return make_scoped_operation(scope,
            detail::map_db_query<result_type>(scope, std::move(task_value), resource, std::move(mapper)));
    }
    template <typename driver_type>
    friend class detail::db_pool;

    struct lease_type final {
        lease_type(detail::db_pool_ref_type client, std::size_t slot, std::pmr::memory_resource* resource,
            operation_options options) noexcept;

        detail::db_pool_ref_type client_;
        std::size_t slot_;
        std::pmr::memory_resource* resource_;
        operation_options options_;
    };

    using operation_state_type = detail::db_operation_state<lease_type>;
    using operation_guard_type = detail::db_operation_guard<lease_type>;

    class state_type;
    using state_owner_type = std::unique_ptr<state_type, pmr_object_deleter<state_type>>;

    db_transaction(detail::db_pool_ref_type client, std::size_t slot, std::pmr::memory_resource* resource,
        operation_options options);
    static task<db_rows> query_prepared(
        std::pmr::string sql, std::pmr::vector<db_value> params, operation_scope& scope, operation_guard_type operation);
    static task<db_exec_result> execute_prepared(
        std::pmr::string sql, std::pmr::vector<db_value> params, operation_scope& scope, operation_guard_type operation);
    static task<void> commit_task(operation_scope& scope, operation_guard_type operation);
    static task<void> rollback_task(operation_scope& scope, operation_guard_type operation);
    scoped_operation<void> run_savepoint_command(std::string_view prefix, std::string_view name);
    static task<void> savepoint_task(std::pmr::string sql, operation_scope& scope, operation_guard_type operation);
    void require_postgresql_transaction(std::string_view feature) const;
    scoped_operation<void> query_void_prepared(std::string_view sql, std::span<const db_value> params);
    scoped_operation<bool> query_bool_prepared(std::string_view sql, std::span<const db_value> params);
    static task<void> query_void_task(
        std::pmr::string sql, std::pmr::vector<db_value> params, operation_scope& scope, operation_guard_type operation);
    static task<void> execute_void_task(
        std::pmr::string sql, std::pmr::vector<db_value> params, operation_scope& scope, operation_guard_type operation);
    static task<bool> query_bool_task(
        std::pmr::string sql, std::pmr::vector<db_value> params, operation_scope& scope, operation_guard_type operation);
    void reset() noexcept;
    void bind_operation_scope(operation_scope& scope) noexcept;
    static void expire_capability(void* target) noexcept;

    [[nodiscard]] detail::db_query_backend query_backend(const lease_type& lease) const;
    template <bool with_count>
    static task<std::conditional_t<with_count, std::pair<db_rows, db_rows>, db_rows>> query_plan_prepared(
        detail::db_query_plan plan, detail::db_query_backend backend, operation_guard_type operation);
    state_owner_type state_;
    scoped_capability_registration registration_;
};

#ifdef RUVIA_ENABLE_REDIS
class db_query_result_cache final {
public:
    db_query_result_cache(const db_query_result_cache& other) noexcept;
    db_query_result_cache& operator=(const db_query_result_cache&) = delete;
    // Removes the entries cached under each id passed to cache(), including the
    // get_one/get_exists/get_count results a repository builder derived from it.
    [[nodiscard]] scoped_operation<void> remove(std::span<const std::string_view> ids) const;
    template <typename... ids_type>
        requires(sizeof...(ids_type) > 0 && (std::convertible_to<ids_type&&, std::string_view> && ...))
    [[nodiscard]] scoped_operation<void> remove(ids_type&&... ids) const {
        const std::string_view views[]{std::string_view(ids)...};
        return remove(std::span<const std::string_view>(views));
    }
    [[nodiscard]] scoped_operation<void> clear() const;

private:
    friend class db_handle;
    db_query_result_cache(
        detail::db_query_cache_state& state_value, ::ruvia::operation_scope& scope, operation_options options) noexcept;
    static void expire_capability(void* target) noexcept;
    detail::db_query_cache_state* state_;
    operation_options options_;
    scoped_capability_registration registration_;
};
#endif

class db_handle final {
public:
    db_handle(const db_handle& other) noexcept;
    db_handle& operator=(const db_handle&) = delete;

    [[nodiscard]] db_handle with_options(operation_options options) const;
#ifdef RUVIA_ENABLE_REDIS
    [[nodiscard]] db_query_result_cache query_result_cache() const;
#endif

    template <typename entity_type>
    [[nodiscard]] db_repository<entity_type, db_handle> repository() const;

    scoped_operation<db_rows> query(std::string_view sql, std::span<const db_value> params = {}) const;
    scoped_operation<db_rows> query(
        std::string_view sql, std::initializer_list<db_value> params) const = delete;
    scoped_operation<db_exec_result> execute(
        std::string_view sql, std::span<const db_value> params = {}) const;
    scoped_operation<db_exec_result> execute(
        std::string_view sql, std::initializer_list<db_value> params) const = delete;
    scoped_operation<db_stream_result> query_stream(
        std::string_view sql, std::span<const db_value> params = {}) const;
    scoped_operation<db_stream_result> query_stream(
        std::string_view sql, std::initializer_list<db_value> params) const = delete;

    // Bound parameters as ordinary arguments: query(sql, id, name). Each one is
    // converted to a db_value and cloned into the prepared statement before the
    // call returns -- the span overloads below build the statement synchronously,
    // never inside the returned operation -- so argument temporaries only have to
    // outlive the call itself, not the operation that is awaited afterwards.
    //
    // A caller that already holds a contiguous parameter sequence passes it as a
    // span instead; a span cannot construct a db_value, so it never reaches here.
    // Fixed SQL uses MariaDB placeholders by default; specify postgresql
    // for numbered parameters. Values are owned by the existing operation path.
    template <fixed_string sql, db_driver driver = db_driver::mariadb, typename... params_type>
        requires(detail::db_parameter<params_type> && ...)
    [[nodiscard]] scoped_operation<db_rows> query(params_type&&... params) const {
        detail::validate_db_sql_literal<sql, driver, sizeof...(params_type)>(query_driver());
        return query(sql.view(), std::forward<params_type>(params)...);
    }

    template <fixed_string sql, db_driver driver = db_driver::mariadb, typename... params_type>
        requires(detail::db_parameter<params_type> && ...)
    [[nodiscard]] scoped_operation<db_exec_result> execute(params_type&&... params) const {
        detail::validate_db_sql_literal<sql, driver, sizeof...(params_type)>(query_driver());
        return execute(sql.view(), std::forward<params_type>(params)...);
    }

    template <fixed_string sql, db_driver driver = db_driver::mariadb, typename... params_type>
        requires(detail::db_parameter<params_type> && ...)
    [[nodiscard]] scoped_operation<db_stream_result> query_stream(params_type&&... params) const {
        detail::validate_db_sql_literal<sql, driver, sizeof...(params_type)>(query_driver());
        return query_stream(sql.view(), std::forward<params_type>(params)...);
    }

    template <typename... params_type>
        requires detail::db_parameter_pack<params_type...>
    [[nodiscard]] scoped_operation<db_rows> query(std::string_view sql, params_type&&... params) const {
        const db_value values[]{detail::make_immediate_db_parameter(std::forward<params_type>(params))...};
        return query(sql, std::span<const db_value>(values));
    }

    template <typename... params_type>
        requires detail::db_parameter_pack<params_type...>
    [[nodiscard]] scoped_operation<db_exec_result> execute(
        std::string_view sql, params_type&&... params) const {
        const db_value values[]{detail::make_immediate_db_parameter(std::forward<params_type>(params))...};
        return execute(sql, std::span<const db_value>(values));
    }

    template <typename... params_type>
        requires detail::db_parameter_pack<params_type...>
    [[nodiscard]] scoped_operation<db_stream_result> query_stream(
        std::string_view sql, params_type&&... params) const {
        const db_value values[]{detail::make_immediate_db_parameter(std::forward<params_type>(params))...};
        return query_stream(sql, std::span<const db_value>(values));
    }

    scoped_operation<db_transaction> begin_transaction(
        db_transaction_options options = {}) const;
    scoped_operation<void> notify(std::string_view channel, std::string_view payload = {}) const;

private:
    friend class detail::db_registry;
    template <typename, typename>
    friend class db_repository;
    template <typename, typename>
    friend class db_query_builder;
    template <typename, typename>
    friend class db_write_query_builder;

    [[nodiscard]] db_driver query_driver() const;
    [[nodiscard]] std::pmr::memory_resource* query_resource() const;
    [[nodiscard]] scoped_operation<db_exec_result> execute_query(const db_query& query) const;
    [[nodiscard]] task<db_rows> query_task(const db_query& query) const;
    [[nodiscard]] task<std::pair<db_rows, db_rows>> query_and_count_task(const db_query& query, const db_query& count) const;
    template <typename result_type, typename mapper_type>
    [[nodiscard]] scoped_operation<std::pair<result_type, std::uint64_t>> query_mapped_and_count(const db_query& query, const db_query& count, mapper_type mapper) const {
        registration_.require_active();
        auto& scope = registration_.scope();
        auto task_value = query_and_count_task(query, count);
        return make_scoped_operation(scope,
            detail::map_db_query_and_count<result_type>(scope, std::move(task_value), resource_, std::move(mapper)));
    }
    template <typename result_type, typename mapper_type>
    [[nodiscard]] scoped_operation<result_type> query_mapped(const db_query& query, mapper_type mapper) const {
        registration_.require_active();
        auto& scope = registration_.scope();
        auto task_value = query_task(query);
        return make_scoped_operation(scope,
            detail::map_db_query<result_type>(scope, std::move(task_value), resource_, std::move(mapper)));
    }

    // cache is nullptr when the alias has no query cache; without Redis
    // support it is always nullptr.
    db_handle(detail::db_pool_ref_type client, std::pmr::memory_resource* resource,
        operation_scope& operation_scope, detail::db_query_cache_state* cache) noexcept;
    [[nodiscard]] detail::db_query_backend query_backend() const;
    static task<db_stream_result> query_stream_prepared(detail::db_pool_ref_type client, std::pmr::string sql,
        std::pmr::vector<db_value> params, std::pmr::memory_resource* resource,
        operation_scope& operation_scope, operation_options options);
    static task<db_transaction> begin_transaction_prepared(detail::db_query_backend backend,
        operation_scope& operation_scope, operation_options operation_options,
        db_transaction_options transaction_options);

    detail::db_query_cache_state* cache_{nullptr};
    detail::db_pool_ref_type client_;
    std::pmr::memory_resource* resource_;
    operation_options options_;
    static void expire_capability(void* target) noexcept;
    scoped_capability_registration registration_;
};

#ifdef RUVIA_ENABLE_DATABASE
// One database pool bound to one Ruvia event loop. It is the standalone
// counterpart of context::db(): application-created and attached workers use
// the same worker-local backend without an HTTP application or a special context.
class db_client final {
public:
    db_client(event_loop loop, const db_config& config);
#ifdef RUVIA_ENABLE_REDIS
    // Cache operations borrow an existing Redis capability on the same worker.
    // Connect Redis first and shut this client down before its Redis owner.
    db_client(event_loop loop, const db_config& config, const redis_handle& cache_store,
        const db_cache_config& cache_policy);
#endif
    ~db_client();

    db_client(const db_client&) = delete;
    db_client& operator=(const db_client&) = delete;
    db_client(db_client&&) = delete;
    db_client& operator=(db_client&&) = delete;

    // Lazy and worker-affine: start/await it on the bound event loop. Creating
    // more than one connect task is harmless; the first task that starts owns
    // the one allowed connection attempt and later starters fail.
    [[nodiscard]] task<void> connect() &;
    task<void> connect() && = delete;

    // Handles and operations borrow this client's open lifecycle. A temporary
    // client would begin pool shutdown at the end of the full expression.
    // Each call registers one allocation-free capability with the client
    // operation scope, so keep the returned handle for a sequence of commands
    // instead of calling handle() per command.
    [[nodiscard]] db_handle handle(operation_options options = {}) const&;
    db_handle handle(operation_options = {}) const&& = delete;

    // Idempotent and callable from any thread. Pool teardown runs on the bound
    // event loop; use shutdown() when its completion must be awaited.
    void close() noexcept;
    // Requests cancellation, joins worker-owned operations, and completes on
    // the bound event loop after the client teardown is finished.
    [[nodiscard]] task<void> shutdown() &;
    task<void> shutdown() && = delete;

    [[nodiscard]] const worker_handle& worker() const& noexcept;
    const worker_handle& worker() const&& = delete;

private:
    std::shared_ptr<detail::db_client_state> state_;
};
#endif

}  // namespace ruvia
