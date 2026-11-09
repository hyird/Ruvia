#pragma once

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <memory>
#include <memory_resource>
#include <span>
#include <stdexcept>
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
#include "ruvia/web/detail/db/db_sql_scan.h"
#include "ruvia/web/fixed_string.h"

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
struct request_io_metrics;

// The outer scoped operation owns both the driver task and the mapped result.
// Decoding finishes before db_rows releases its backend storage. Scalar and
// column decoders enforce db_mapped_value and construct owning fields in the
// same operation memory domain; raw mappers transfer db_rows itself.
template <typename result_type, typename mapper_type>
task<result_type> map_db_query(task<db_rows> query, std::pmr::memory_resource* resource, mapper_type mapper) {
    auto rows = co_await std::move(query);
    co_return mapper(std::move(rows), resource);
}

inline std::uint64_t db_count_value(const db_rows& rows) {
    if (rows.size() != 1) {
        throw db_conversion_error(
            db_conversion_error::code_type::unexpected_row_count, "database count did not return exactly one row");
    }
    const auto count = rows.front()["count"].as<std::uint64_t>();
    if (!count) {
        throw db_conversion_error(db_conversion_error::code_type::unexpected_null, "database count column is NULL");
    }
    return *count;
}

template <typename result_type, typename mapper_type>
task<std::pair<result_type, std::uint64_t>> map_db_query_and_count(
    task<std::pair<db_rows, db_rows>> query, std::pmr::memory_resource* resource, mapper_type mapper) {
    auto rows = co_await std::move(query);
    const auto count = db_count_value(rows.second);
    co_return std::pair{mapper(std::move(rows.first), resource), count};
}

struct db_sql_parameter_count {
    std::size_t count_{0};
    bool valid_{true};
};

// Count MariaDB slots, or PostgreSQL's highest numbered parameter. Repeated
// PostgreSQL references bind the same value; '?' remains a PostgreSQL operator.
// This checks the binding contract, not SQL grammar or schema correctness.
[[nodiscard]] constexpr db_sql_parameter_count count_db_sql_parameters(std::string_view sql, db_driver driver) noexcept {
    db_sql_parameter_count result;
    if (driver != db_driver::mariadb && driver != db_driver::postgresql) {
        return {0, false};
    }
    for (std::size_t index = 0; index < sql.size();) {
        if (sql[index] == '\0') {
            return {0, false};
        }
        const auto next_value =
            driver == db_driver::mariadb ? skip_sql_atom(sql, index, true) : skip_postgresql_sql_atom(sql, index);
        if (next_value != index + 1) {
            index = next_value;
            continue;
        }
        if (driver == db_driver::mariadb) {
            result.count_ += sql[index] == '?';
            ++index;
            continue;
        }
        if (sql[index] == '$' && index + 1 < sql.size() && sql[index + 1] >= '0' && sql[index + 1] <= '9') {
            std::size_t number = 0;
            ++index;
            while (index < sql.size() && sql[index] >= '0' && sql[index] <= '9') {
                const auto digit = static_cast<unsigned>(sql[index++] - '0');
                if (number > ((std::numeric_limits<std::size_t>::max)() - digit) / 10) {
                    return {0, false};
                }
                number = number * 10 + digit;
            }
            if (number == 0) {
                return {0, false};
            }
            if (number > result.count_) {
                result.count_ = number;
            }
            continue;
        }
        // Unquoted identifiers can contain '$'; account$1 is not a parameter.
        if (is_postgresql_dollar_tag_start(sql[index]) || static_cast<unsigned char>(sql[index]) >= 0x80) {
            do {
                ++index;
            } while (index < sql.size() && (is_postgresql_identifier_continue(sql[index]) || static_cast<unsigned char>(sql[index]) >= 0x80));
        } else {
            ++index;
        }
    }
    return result;
}

template <fixed_string sql, db_driver driver, std::size_t parameter_count>
void validate_db_sql_literal(db_driver actual_driver) {
    static_assert(driver == db_driver::unspecified || driver == db_driver::mariadb || driver == db_driver::postgresql,
        "SQL literal requires a valid database driver");
    static_assert(sql.view().find('\0') == std::string_view::npos, "SQL literal must not contain NUL bytes");
    if constexpr (driver == db_driver::unspecified) {
        constexpr auto mariadb_result = count_db_sql_parameters(sql.view(), db_driver::mariadb);
        constexpr auto postgresql_result = count_db_sql_parameters(sql.view(), db_driver::postgresql);
        constexpr bool has_mariadb = mariadb_result.count_ > 0;
        constexpr bool has_postgresql = postgresql_result.count_ > 0 || !postgresql_result.valid_;
        static_assert(!(has_mariadb && has_postgresql),
            "SQL literal with both '?' and '$n' requires an explicit database driver");
        if constexpr (has_postgresql) {
            static_assert(postgresql_result.valid_, "SQL literal contains an invalid parameter index");
            static_assert(postgresql_result.count_ == parameter_count,
                "SQL placeholder count does not match the number of arguments");
            if (actual_driver != db_driver::postgresql) {
                throw std::invalid_argument("SQL literal dialect does not match the database driver");
            }
        } else if constexpr (has_mariadb) {
            static_assert(mariadb_result.valid_, "SQL literal contains an invalid parameter index");
            static_assert(mariadb_result.count_ == parameter_count,
                "SQL placeholder count does not match the number of arguments");
            if (actual_driver != db_driver::mariadb) {
                throw std::invalid_argument("SQL literal dialect does not match the database driver");
            }
        } else {
            static_assert(parameter_count == 0,
                "SQL placeholder count does not match the number of arguments");
            if (actual_driver != db_driver::mariadb && actual_driver != db_driver::postgresql) {
                throw std::invalid_argument("SQL literal dialect does not match the database driver");
            }
        }
    } else {
        constexpr auto result_value = count_db_sql_parameters(sql.view(), driver);
        static_assert(result_value.valid_, "SQL literal contains an invalid parameter index");
        static_assert(result_value.count_ == parameter_count,
            "SQL placeholder count does not match the number of arguments");
        if (actual_driver != driver) {
            throw std::invalid_argument("SQL literal dialect does not match the database driver");
        }
    }
}
}  // namespace detail

// Statement and completion operations are lazy. Inputs are owned before the
// entry returns, so cold operations may be prepared while a statement runs.
// Only starting overlapping operations throws std::logic_error; that rejected
// operation does not abort the statement already holding the transaction.
//
// The transaction holds its connection under db_config::transaction_timeout_,
// counted from begin_transaction() holding the connection, including the time
// between operations. Each operation runs under the earlier of its own
// deadline and the remaining transaction time. Once that has passed while no
// operation runs, the connection is closed, so the server rolls back and
// releases the transaction's locks; the next operation (commit() and
// rollback() included) fails with db_error::code_type::timeout and ends the
// transaction.
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

    [[nodiscard]] task<db_rows> query(std::string_view sql, std::span<const db_value> params = {}) &;
    task<db_rows> query(std::string_view, std::span<const db_value> = {}) && = delete;
    task<db_rows> query(std::string_view, std::initializer_list<db_value>) & = delete;
    task<db_rows> query(std::string_view, std::initializer_list<db_value>) && = delete;
    [[nodiscard]] task<db_exec_result> execute(
        std::string_view sql, std::span<const db_value> params = {}) &;
    task<db_exec_result> execute(
        std::string_view, std::span<const db_value> = {}) && = delete;
    task<db_exec_result> execute(
        std::string_view, std::initializer_list<db_value>) & = delete;
    task<db_exec_result> execute(
        std::string_view, std::initializer_list<db_value>) && = delete;

    // Bound parameters as ordinary arguments, with the same synchronous cloning
    // and the same temporary-safety as db_handle::query()/execute().
    // Fixed SQL uses MariaDB placeholders by default; specify postgresql
    // for numbered parameters. Values are owned by the existing operation path.
    template <fixed_string sql, db_driver driver = db_driver::unspecified, typename... params_type>
        requires(detail::db_parameter<params_type> && ...)
    [[nodiscard]] task<db_rows> query(params_type&&... params) & {
        detail::validate_db_sql_literal<sql, driver, sizeof...(params_type)>(query_driver());
        return query(sql.view(), std::forward<params_type>(params)...);
    }
    template <fixed_string sql, db_driver driver = db_driver::unspecified, typename... params_type>
        requires(detail::db_parameter<params_type> && ...)
    task<db_rows> query(params_type&&...) && = delete;

    template <fixed_string sql, db_driver driver = db_driver::unspecified, typename... params_type>
        requires(detail::db_parameter<params_type> && ...)
    [[nodiscard]] task<db_exec_result> execute(params_type&&... params) & {
        detail::validate_db_sql_literal<sql, driver, sizeof...(params_type)>(query_driver());
        return execute(sql.view(), std::forward<params_type>(params)...);
    }
    template <fixed_string sql, db_driver driver = db_driver::unspecified, typename... params_type>
        requires(detail::db_parameter<params_type> && ...)
    task<db_exec_result> execute(params_type&&...) && = delete;

    template <typename... params_type>
        requires detail::db_parameter_pack<params_type...>
    [[nodiscard]] task<db_rows> query(std::string_view sql, params_type&&... params) & {
        const db_value values[]{detail::make_immediate_db_parameter(std::forward<params_type>(params))...};
        return query(sql, std::span<const db_value>(values));
    }
    template <typename... params_type>
        requires detail::db_parameter_pack<params_type...>
    task<db_rows> query(std::string_view, params_type&&...) && = delete;

    template <typename... params_type>
        requires detail::db_parameter_pack<params_type...>
    [[nodiscard]] task<db_exec_result> execute(
        std::string_view sql, params_type&&... params) & {
        const db_value values[]{detail::make_immediate_db_parameter(std::forward<params_type>(params))...};
        return execute(sql, std::span<const db_value>(values));
    }
    template <typename... params_type>
        requires detail::db_parameter_pack<params_type...>
    task<db_exec_result> execute(std::string_view, params_type&&...) && = delete;

    [[nodiscard]] task<void> commit() &;
    task<void> commit() && = delete;
    [[nodiscard]] task<void> rollback() &;
    task<void> rollback() && = delete;
    [[nodiscard]] task<void> savepoint(std::string_view name) &;
    task<void> savepoint(std::string_view) && = delete;
    [[nodiscard]] task<void> rollback_to_savepoint(std::string_view name) &;
    task<void> rollback_to_savepoint(std::string_view) && = delete;
    [[nodiscard]] task<void> release_savepoint(std::string_view name) &;
    task<void> release_savepoint(std::string_view) && = delete;
    [[nodiscard]] task<void> notify(std::string_view channel, std::string_view payload = {}) &;
    task<void> notify(std::string_view, std::string_view = {}) && = delete;
    [[nodiscard]] task<void> advisory_lock(std::int64_t key) &;
    task<void> advisory_lock(std::int64_t) && = delete;
    [[nodiscard]] task<void> advisory_lock(std::int32_t class_id, std::int32_t object_id) &;
    task<void> advisory_lock(std::int32_t, std::int32_t) && = delete;
    [[nodiscard]] task<void> advisory_lock(std::string_view key, std::int64_t seed = 0) &;
    task<void> advisory_lock(std::string_view, std::int64_t = 0) && = delete;
    [[nodiscard]] task<bool> try_advisory_lock(std::int64_t key) &;
    task<bool> try_advisory_lock(std::int64_t) && = delete;
    [[nodiscard]] task<bool> try_advisory_lock(std::int32_t class_id, std::int32_t object_id) &;
    task<bool> try_advisory_lock(std::int32_t, std::int32_t) && = delete;
    [[nodiscard]] task<bool> try_advisory_lock(std::string_view key, std::int64_t seed = 0) &;
    task<bool> try_advisory_lock(std::string_view, std::int64_t = 0) && = delete;
    [[nodiscard]] task<void> declare_cursor(
        std::string_view name, std::string_view sql, std::span<const db_value> params = {}, bool scroll = false) &;
    task<void> declare_cursor(
        std::string_view, std::string_view, std::span<const db_value> = {}, bool = false) && = delete;
    task<void> declare_cursor(
        std::string_view, std::string_view, std::initializer_list<db_value>, bool = false) & = delete;
    task<void> declare_cursor(
        std::string_view, std::string_view, std::initializer_list<db_value>, bool = false) && = delete;
    [[nodiscard]] task<db_rows> fetch_cursor(std::string_view name, std::uint64_t count = 1) &;
    task<db_rows> fetch_cursor(std::string_view, std::uint64_t = 1) && = delete;
    [[nodiscard]] task<void> close_cursor(std::string_view name) &;
    task<void> close_cursor(std::string_view) && = delete;

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
    [[nodiscard]] task<db_exec_result> execute_query(const db_query& query);
    [[nodiscard]] task<db_exec_result> execute_prepared(std::pmr::string sql);
    [[nodiscard]] task<db_rows> query_task(const db_query& query);
    [[nodiscard]] task<std::pair<db_rows, db_rows>> query_and_count_task(const db_query& query, const db_query& count);
    template <typename result_type, typename mapper_type>
    [[nodiscard]] task<std::pair<result_type, std::uint64_t>> query_mapped_and_count(const db_query& query, const db_query& count, mapper_type mapper) {
        registration_.require_active();
        auto& scope = registration_.scope();
        auto* resource = query_resource();
        auto task_value = query_and_count_task(query, count);
        return make_scoped_operation(scope,
            detail::map_db_query_and_count<result_type>(std::move(task_value), resource, std::move(mapper)));
    }
    template <typename result_type, typename mapper_type>
    [[nodiscard]] task<result_type> query_mapped(const db_query& query, mapper_type mapper) {
        registration_.require_active();
        auto& scope = registration_.scope();
        auto* resource = query_resource();
        auto task_value = query_task(query);
        return make_scoped_operation(scope,
            detail::map_db_query<result_type>(std::move(task_value), resource, std::move(mapper)));
    }
    template <typename driver_type>
    friend class detail::db_pool;

    struct lease_type final {
        lease_type(detail::db_pool_ref_type client, std::size_t slot, std::pmr::memory_resource* resource,
            operation_options options) noexcept;
        void abort() noexcept;

        detail::db_pool_ref_type client_;
        std::size_t slot_;
        std::pmr::memory_resource* resource_;
        operation_options options_;
    };

    using operation_state_type = detail::db_operation_state<lease_type>;
    using operation_guard_type = detail::db_operation_guard<lease_type>;

    using state_owner_type = std::unique_ptr<operation_state_type, pmr_object_deleter<operation_state_type>>;

    db_transaction(detail::db_pool_ref_type client, std::size_t slot, std::pmr::memory_resource* resource,
        operation_options options);
    template <typename result_type, bool exec_statement>
    task<result_type> run_statement(std::pmr::string sql, std::pmr::vector<db_value> params);
    // `io_metrics` is the request's counter block, or null outside a request.
    template <typename result_type, bool exec_statement>
    static task<result_type> statement_task(std::pmr::string sql, std::pmr::vector<db_value> params,
        detail::request_io_metrics* io_metrics, operation_guard_type operation);
    template <bool commit_lease>
    static task<void> finish_task(detail::request_io_metrics* io_metrics, operation_guard_type operation);
    // A command naming one identifier: SAVEPOINT, RELEASE, CLOSE and the like.
    task<void> run_identifier_command(std::string_view prefix, std::string_view name);
    void require_postgresql_transaction(std::string_view feature) const;
    task<void> query_void_prepared(std::string_view sql, std::span<const db_value> params);
    task<bool> query_bool_prepared(std::string_view sql, std::span<const db_value> params);
    void reset() noexcept;
    void bind_operation_scope(operation_scope& scope, detail::request_io_metrics* io_metrics) noexcept;
    static void expire_capability(void* target) noexcept;

    [[nodiscard]] detail::db_query_backend query_backend(const lease_type& lease) const;
    template <bool with_count>
    static task<std::conditional_t<with_count, std::pair<db_rows, db_rows>, db_rows>> query_plan_prepared(
        detail::db_query_plan plan, detail::db_query_backend backend, operation_guard_type operation);
    state_owner_type state_;
    detail::request_io_metrics* io_metrics_{nullptr};
    scoped_capability_registration registration_;
};

#ifdef RUVIA_ENABLE_REDIS
class db_query_result_cache final {
public:
    db_query_result_cache(const db_query_result_cache& other) noexcept;
    db_query_result_cache& operator=(const db_query_result_cache&) = delete;
    // Removes the entries cached under each id passed to cache(), including the
    // get_one/get_exists/get_count results a repository builder derived from it.
    [[nodiscard]] task<void> remove(std::span<const std::string_view> ids) const;
    template <typename... ids_type>
        requires(sizeof...(ids_type) > 0 && (std::convertible_to<ids_type&&, std::string_view> && ...))
    [[nodiscard]] task<void> remove(ids_type&&... ids) const {
        const std::string_view views[]{std::string_view(ids)...};
        return remove(std::span<const std::string_view>(views));
    }
    [[nodiscard]] task<void> clear() const;

private:
    friend class db_handle;
    db_query_result_cache(detail::db_query_cache_state& state_value, ::ruvia::operation_scope& scope,
        detail::request_io_metrics* io_metrics, operation_options options) noexcept;
    static void expire_capability(void* target) noexcept;
    detail::db_query_cache_state* state_;
    detail::request_io_metrics* io_metrics_;
    operation_options options_;
    scoped_capability_registration registration_;
};
#endif

class db_handle final {
public:
    db_handle(const db_handle& other) noexcept;
    db_handle& operator=(const db_handle&) = delete;

    // Inherits an empty token; a supplied state replaces user cancellation.
    // Timeout can only tighten. Accumulation requires a caller-owned stop_group.
    [[nodiscard]] db_handle with_options(operation_options options) const;
    [[nodiscard]] const worker_handle& worker() const& RUVIA_LIFETIMEBOUND;
    const worker_handle& worker() const&& = delete;
#ifdef RUVIA_ENABLE_REDIS
    [[nodiscard]] db_query_result_cache query_result_cache() const;
#endif

    template <typename entity_type>
    [[nodiscard]] db_repository<entity_type, db_handle> repository() const;

    [[nodiscard]] task<db_rows> query(std::string_view sql, std::span<const db_value> params = {}) const;
    task<db_rows> query(
        std::string_view sql, std::initializer_list<db_value> params) const = delete;
    [[nodiscard]] task<db_exec_result> execute(
        std::string_view sql, std::span<const db_value> params = {}) const;
    task<db_exec_result> execute(
        std::string_view sql, std::initializer_list<db_value> params) const = delete;
    [[nodiscard]] task<db_stream_result> query_stream(
        std::string_view sql, std::span<const db_value> params = {}) const;
    task<db_stream_result> query_stream(
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
    template <fixed_string sql, db_driver driver = db_driver::unspecified, typename... params_type>
        requires(detail::db_parameter<params_type> && ...)
    [[nodiscard]] task<db_rows> query(params_type&&... params) const {
        detail::validate_db_sql_literal<sql, driver, sizeof...(params_type)>(query_driver());
        return query(sql.view(), std::forward<params_type>(params)...);
    }

    template <fixed_string sql, db_driver driver = db_driver::unspecified, typename... params_type>
        requires(detail::db_parameter<params_type> && ...)
    [[nodiscard]] task<db_exec_result> execute(params_type&&... params) const {
        detail::validate_db_sql_literal<sql, driver, sizeof...(params_type)>(query_driver());
        return execute(sql.view(), std::forward<params_type>(params)...);
    }

    template <fixed_string sql, db_driver driver = db_driver::unspecified, typename... params_type>
        requires(detail::db_parameter<params_type> && ...)
    [[nodiscard]] task<db_stream_result> query_stream(params_type&&... params) const {
        detail::validate_db_sql_literal<sql, driver, sizeof...(params_type)>(query_driver());
        return query_stream(sql.view(), std::forward<params_type>(params)...);
    }

    template <typename... params_type>
        requires detail::db_parameter_pack<params_type...>
    [[nodiscard]] task<db_rows> query(std::string_view sql, params_type&&... params) const {
        const db_value values[]{detail::make_immediate_db_parameter(std::forward<params_type>(params))...};
        return query(sql, std::span<const db_value>(values));
    }

    template <typename... params_type>
        requires detail::db_parameter_pack<params_type...>
    [[nodiscard]] task<db_exec_result> execute(
        std::string_view sql, params_type&&... params) const {
        const db_value values[]{detail::make_immediate_db_parameter(std::forward<params_type>(params))...};
        return execute(sql, std::span<const db_value>(values));
    }

    template <typename... params_type>
        requires detail::db_parameter_pack<params_type...>
    [[nodiscard]] task<db_stream_result> query_stream(
        std::string_view sql, params_type&&... params) const {
        const db_value values[]{detail::make_immediate_db_parameter(std::forward<params_type>(params))...};
        return query_stream(sql, std::span<const db_value>(values));
    }

    [[nodiscard]] task<db_transaction> begin_transaction(
        db_transaction_options options = {}) const;
    [[nodiscard]] task<void> notify(std::string_view channel, std::string_view payload = {}) const;

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
    [[nodiscard]] task<db_exec_result> execute_query(const db_query& query) const;
    [[nodiscard]] task<db_exec_result> execute_prepared(std::pmr::string sql) const;
    [[nodiscard]] task<db_rows> query_task(const db_query& query) const;
    [[nodiscard]] task<std::pair<db_rows, db_rows>> query_and_count_task(const db_query& query, const db_query& count) const;
    template <typename result_type, typename mapper_type>
    [[nodiscard]] task<std::pair<result_type, std::uint64_t>> query_mapped_and_count(const db_query& query, const db_query& count, mapper_type mapper) const {
        registration_.require_active();
        auto& scope = registration_.scope();
        auto task_value = query_and_count_task(query, count);
        return make_scoped_operation(scope,
            detail::map_db_query_and_count<result_type>(std::move(task_value), resource_, std::move(mapper)));
    }
    template <typename result_type, typename mapper_type>
    [[nodiscard]] task<result_type> query_mapped(const db_query& query, mapper_type mapper) const {
        registration_.require_active();
        auto& scope = registration_.scope();
        auto task_value = query_task(query);
        return make_scoped_operation(scope,
            detail::map_db_query<result_type>(std::move(task_value), resource_, std::move(mapper)));
    }

    // cache is nullptr when the alias has no query cache; without Redis
    // support it is always nullptr. `io_metrics` is the request's counter
    // block, or null outside a request.
    db_handle(detail::db_pool_ref_type client, std::pmr::memory_resource* resource,
        operation_scope& operation_scope, detail::request_io_metrics* io_metrics,
        detail::db_query_cache_state* cache) noexcept;
    [[nodiscard]] detail::db_query_backend query_backend() const;
    static task<db_stream_result> query_stream_prepared(detail::db_pool_ref_type client, std::pmr::string sql,
        std::pmr::vector<db_value> params, std::pmr::memory_resource* resource,
        operation_scope& operation_scope, detail::request_io_metrics* io_metrics, operation_options options);
    static task<db_transaction> begin_transaction_prepared(detail::db_query_backend backend,
        operation_scope& operation_scope, operation_options operation_options,
        db_transaction_options transaction_options);

    detail::db_query_cache_state* cache_{nullptr};
    detail::db_pool_ref_type client_;
    std::pmr::memory_resource* resource_;
    detail::request_io_metrics* io_metrics_{nullptr};
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
    // Pending pool waiters report db_error::code_type::closing, distinct from
    // user-token cancellation (db_error::code_type::cancelled).
    // Requests cancellation, joins worker-owned operations, and completes on
    // the bound event loop after the client teardown is finished.
    [[nodiscard]] task<void> shutdown() &;
    task<void> shutdown() && = delete;

    [[nodiscard]] const worker_handle& worker() const& noexcept RUVIA_LIFETIMEBOUND;
    const worker_handle& worker() const&& = delete;

private:
    std::shared_ptr<detail::db_client_state> state_;
};
#endif

}  // namespace ruvia
