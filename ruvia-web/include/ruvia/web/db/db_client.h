#pragma once

#include <concepts>
#include <initializer_list>
#include <memory>
#include <span>
#include <string_view>
#include <type_traits>
#include <utility>

#include "ruvia/core/event_loop.h"
#include "ruvia/core/task.h"
#include "ruvia/web/db/db_cache.h"
#include "ruvia/web/db/db_handle.h"
#include "ruvia/web/db/db_repository.h"
#include "ruvia/web/db/db_types.h"
#include "ruvia/web/detail/db/db_sql_literal.h"

namespace ruvia {

class redis_handle;

namespace detail {
class db_client_state;
}

// One database pool bound to one Ruvia event loop. It is the standalone
// counterpart of context::db(): application-created and attached workers use
// the same worker-local backend without an HTTP application or a special context.
class db_client final {
public:
    db_client(event_loop loop, const db_config& config);
    // Cache operations borrow an existing Redis capability on the same worker.
    // Connect Redis first and shut this client down before its Redis owner.
    db_client(event_loop loop, const db_config& config, const redis_handle& cache_store,
        const db_cache_config& cache_policy);
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
    [[nodiscard]] db_handle with_options(operation_options options) const&;
    db_handle with_options(operation_options) const&& = delete;

    [[nodiscard]] db_query_result_cache query_result_cache() const& {
        return with_options({}).query_result_cache();
    }
    db_query_result_cache query_result_cache() const&& = delete;

    template <typename entity_type>
    [[nodiscard]] db_repository<entity_type, db_handle> get_repository() const& {
        return with_options({}).template get_repository<entity_type>();
    }
    template <typename entity_type>
    db_repository<entity_type, db_handle> get_repository() const&& = delete;

    [[nodiscard]] scoped_operation<db_rows> query(const db_query& query) const& {
        return with_options({}).query(query);
    }
    scoped_operation<db_rows> query(const db_query&) const&& = delete;
    [[nodiscard]] scoped_operation<db_exec_result> execute(const db_query& query) const& {
        return with_options({}).execute(query);
    }
    scoped_operation<db_exec_result> execute(const db_query&) const&& = delete;

    [[nodiscard]] scoped_operation<db_rows> query(
        std::string_view sql, std::span<const db_value> params = {}) const&;
    scoped_operation<db_rows> query(std::string_view, std::span<const db_value> = {}) const&& = delete;
    [[nodiscard]] scoped_operation<db_rows> query(
        std::string_view, std::initializer_list<db_value>) const& = delete;
    scoped_operation<db_rows> query(
        std::string_view, std::initializer_list<db_value>) const&& = delete;

    [[nodiscard]] scoped_operation<db_exec_result> execute(
        std::string_view sql, std::span<const db_value> params = {}) const&;
    scoped_operation<db_exec_result> execute(
        std::string_view, std::span<const db_value> = {}) const&& = delete;
    [[nodiscard]] scoped_operation<db_exec_result> execute(
        std::string_view, std::initializer_list<db_value>) const& = delete;
    scoped_operation<db_exec_result> execute(
        std::string_view, std::initializer_list<db_value>) const&& = delete;

    [[nodiscard]] scoped_operation<db_stream_result> query_stream(
        std::string_view sql, std::span<const db_value> params = {}) const&;
    scoped_operation<db_stream_result> query_stream(
        std::string_view, std::span<const db_value> = {}) const&& = delete;
    [[nodiscard]] scoped_operation<db_stream_result> query_stream(
        std::string_view, std::initializer_list<db_value>) const& = delete;
    scoped_operation<db_stream_result> query_stream(
        std::string_view, std::initializer_list<db_value>) const&& = delete;

    // Fixed SQL uses MariaDB placeholders by default; specify postgresql
    // for numbered parameters. Values are owned by the existing operation path.
    template <fixed_string sql, db_driver driver = db_driver::mariadb, typename... params_type>
        requires(detail::db_parameter<params_type> && ...)
    [[nodiscard]] scoped_operation<db_rows> query(params_type&&... params) const& {
        return with_options({}).template query<sql, driver>(std::forward<params_type>(params)...);
    }
    template <fixed_string sql, db_driver driver = db_driver::mariadb, typename... params_type>
        requires(detail::db_parameter<params_type> && ...)
    scoped_operation<db_rows> query(params_type&&...) const&& = delete;

    template <fixed_string sql, db_driver driver = db_driver::mariadb, typename... params_type>
        requires(detail::db_parameter<params_type> && ...)
    [[nodiscard]] scoped_operation<db_exec_result> execute(params_type&&... params) const& {
        return with_options({}).template execute<sql, driver>(std::forward<params_type>(params)...);
    }
    template <fixed_string sql, db_driver driver = db_driver::mariadb, typename... params_type>
        requires(detail::db_parameter<params_type> && ...)
    scoped_operation<db_exec_result> execute(params_type&&...) const&& = delete;

    template <fixed_string sql, db_driver driver = db_driver::mariadb, typename... params_type>
        requires(detail::db_parameter<params_type> && ...)
    [[nodiscard]] scoped_operation<db_stream_result> query_stream(params_type&&... params) const& {
        return with_options({}).template query_stream<sql, driver>(std::forward<params_type>(params)...);
    }
    template <fixed_string sql, db_driver driver = db_driver::mariadb, typename... params_type>
        requires(detail::db_parameter<params_type> && ...)
    scoped_operation<db_stream_result> query_stream(params_type&&...) const&& = delete;

    template <typename... params_type>
        requires detail::db_parameter_pack<params_type...>
    [[nodiscard]] scoped_operation<db_rows> query(std::string_view sql, params_type&&... params) const& {
        return with_options({}).query(sql, std::forward<params_type>(params)...);
    }
    template <typename... params_type>
        requires detail::db_parameter_pack<params_type...>
    scoped_operation<db_rows> query(std::string_view, params_type&&...) const&& = delete;

    template <typename... params_type>
        requires detail::db_parameter_pack<params_type...>
    [[nodiscard]] scoped_operation<db_exec_result> execute(
        std::string_view sql, params_type&&... params) const& {
        return with_options({}).execute(sql, std::forward<params_type>(params)...);
    }
    template <typename... params_type>
        requires detail::db_parameter_pack<params_type...>
    scoped_operation<db_exec_result> execute(std::string_view, params_type&&...) const&& = delete;

    template <typename... params_type>
        requires detail::db_parameter_pack<params_type...>
    [[nodiscard]] scoped_operation<db_stream_result> query_stream(
        std::string_view sql, params_type&&... params) const& {
        return with_options({}).query_stream(sql, std::forward<params_type>(params)...);
    }
    template <typename... params_type>
        requires detail::db_parameter_pack<params_type...>
    scoped_operation<db_stream_result> query_stream(std::string_view, params_type&&...) const&& = delete;

    [[nodiscard]] scoped_operation<db_transaction> begin_transaction(
        db_transaction_options options = {}) const&;
    scoped_operation<db_transaction> begin_transaction(db_transaction_options = {}) const&& = delete;

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

}  // namespace ruvia
