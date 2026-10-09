#pragma once

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

#include "ruvia/core/memory/pmr_object.h"
#include "ruvia/core/scoped_operation.h"
#include "ruvia/web/db/db_rows.h"
#include "ruvia/web/db/db_types.h"
#include "ruvia/web/detail/db/db_mapped_query.h"
#include "ruvia/web/detail/db/db_parameter_pack.h"
#include "ruvia/web/detail/db/db_sql_literal.h"

namespace ruvia {

class db_transaction;
class db_query;
class db_statement;
template <typename entity_type>
class entity_rows;
template <typename entity_type, typename executor_type>
class db_repository;
template <typename entity_type, typename executor_type>
class db_query_builder;

namespace detail {
class db_query_cache_state;
struct db_query_plan;
struct db_transaction_start_plan;
template <typename pool_type>
task<db_transaction> begin_db_transaction(pool_type&, std::pmr::memory_resource*,
    operation_options, db_transaction_start_plan);
}  // namespace detail

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
    [[nodiscard]] db_repository<entity_type, db_transaction> get_repository() &;
    template <typename entity_type>
    db_repository<entity_type, db_transaction> get_repository() && = delete;

    [[nodiscard]] scoped_operation<db_rows> query(const db_query& query) &;
    scoped_operation<db_rows> query(const db_query&) && = delete;
    [[nodiscard]] scoped_operation<db_exec_result> execute(const db_query& query) &;
    scoped_operation<db_exec_result> execute(const db_query&) && = delete;
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
    [[nodiscard]] task<db_rows> query_task(const db_query& query);
    [[nodiscard]] task<std::pair<db_rows, db_rows>> query_and_count_task(const db_query& query, const db_query& count);
    template <typename result_type, typename mapper_type>
    [[nodiscard]] scoped_operation<std::pair<result_type, std::uint64_t>> query_mapped_and_count(const db_query& query, const db_query& count, mapper_type mapper) {
        registration_.require_active();
        auto* resource = query_resource();
        auto task_value = query_and_count_task(query, count);
        return make_scoped_operation(registration_.scope(),
            detail::map_db_query_and_count<result_type>(std::move(task_value), resource, std::move(mapper)));
    }
    template <typename result_type, typename mapper_type>
    [[nodiscard]] scoped_operation<result_type> query_mapped(const db_query& query, mapper_type mapper) {
        registration_.require_active();
        auto* resource = query_resource();
        auto task_value = query_task(query);
        return make_scoped_operation(registration_.scope(),
            detail::map_db_query<result_type>(std::move(task_value), resource, std::move(mapper)));
    }
    friend class detail::mariadb_pool;
    friend class detail::postgresql_pool;
    template <typename pool_type>
    friend task<db_transaction> detail::begin_db_transaction(
        pool_type&, std::pmr::memory_resource*, operation_options,
        detail::db_transaction_start_plan);

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
    using state_owner_type = std::unique_ptr<state_type, detail::pmr_object_deleter<state_type>>;

    db_transaction(detail::db_pool_ref_type client, std::size_t slot, std::pmr::memory_resource* resource,
        operation_options options);
    static task<db_rows> query_prepared(
        std::pmr::string sql, std::pmr::vector<db_value> params, operation_guard_type operation);
    static task<db_exec_result> execute_prepared(
        std::pmr::string sql, std::pmr::vector<db_value> params, operation_guard_type operation);
    static task<void> commit_task(operation_guard_type operation);
    static task<void> rollback_task(operation_guard_type operation);
    void reset() noexcept;
    void bind_operation_scope(operation_scope& scope) noexcept;
    static void expire_capability(void* target) noexcept;

    template <bool with_count>
    static task<std::conditional_t<with_count, std::pair<db_rows, db_rows>, db_rows>> query_plan_prepared(
        detail::db_query_plan plan, detail::db_query_cache_state* cache, operation_guard_type operation);
    detail::db_query_cache_state* cache_{nullptr};
    state_owner_type state_;
    scoped_capability_registration registration_;
};

}  // namespace ruvia
