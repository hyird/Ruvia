#pragma once

#include <cstdint>
#include <initializer_list>
#include <memory_resource>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ruvia/core/scoped_operation.h"
#include "ruvia/web/db/db_query_result_cache.h"
#include "ruvia/web/db/db_transaction.h"
#include "ruvia/web/detail/db/db_backend.h"
#include "ruvia/web/detail/db/db_mapped_query.h"
#include "ruvia/web/detail/db/db_parameter_pack.h"
#include "ruvia/web/detail/db/db_sql_literal.h"

namespace ruvia {

class db_query;
template <typename entity_type>
class entity_rows;
template <typename entity_type, typename executor_type>
class db_repository;
template <typename entity_type, typename executor_type>
class db_query_builder;

class db_handle final {
public:
    db_handle(const db_handle& other) noexcept;
    db_handle& operator=(const db_handle&) = delete;

    [[nodiscard]] db_handle with_options(operation_options options) const;
    [[nodiscard]] db_query_result_cache query_result_cache() const;

    template <typename entity_type>
    [[nodiscard]] db_repository<entity_type, db_handle> get_repository() const;

    [[nodiscard]] scoped_operation<db_rows> query(const db_query& query) const;
    [[nodiscard]] scoped_operation<db_exec_result> execute(const db_query& query) const;

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
    [[nodiscard]] task<db_rows> query_task(const db_query& query) const;
    [[nodiscard]] task<std::pair<db_rows, db_rows>> query_and_count_task(const db_query& query, const db_query& count) const;
    template <typename result_type, typename mapper_type>
    [[nodiscard]] scoped_operation<std::pair<result_type, std::uint64_t>> query_mapped_and_count(const db_query& query, const db_query& count, mapper_type mapper) const {
        registration_.require_active();
        auto task_value = query_and_count_task(query, count);
        return make_scoped_operation(registration_.scope(),
            detail::map_db_query_and_count<result_type>(std::move(task_value), resource_, std::move(mapper)));
    }
    template <typename result_type, typename mapper_type>
    [[nodiscard]] scoped_operation<result_type> query_mapped(const db_query& query, mapper_type mapper) const {
        registration_.require_active();
        auto task_value = query_task(query);
        return make_scoped_operation(registration_.scope(),
            detail::map_db_query<result_type>(std::move(task_value), resource_, std::move(mapper)));
    }

    db_handle(detail::db_pool_ref_type client, std::pmr::memory_resource* resource,
        operation_scope& operation_scope, detail::db_query_cache_state* cache = nullptr) noexcept;
    static task<db_stream_result> query_stream_prepared(detail::db_pool_ref_type client, std::pmr::string sql,
        std::pmr::vector<db_value> params, std::pmr::memory_resource* resource,
        operation_scope& operation_scope, operation_options options);
    static task<db_transaction> begin_transaction_prepared(detail::db_pool_ref_type client,
        std::pmr::memory_resource* resource, operation_scope& operation_scope,
        operation_options operation_options, db_transaction_options transaction_options, detail::db_query_cache_state* cache);

    detail::db_query_cache_state* cache_{nullptr};
    detail::db_pool_ref_type client_;
    std::pmr::memory_resource* resource_;
    operation_options options_;
    static void expire_capability(void* target) noexcept;
    scoped_capability_registration registration_;
};

}  // namespace ruvia
