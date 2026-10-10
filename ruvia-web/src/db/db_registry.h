#pragma once

#include "ruvia/core/operation_deadline.h"
#include "ruvia/core/worker_handle.h"
#include "ruvia/core/worker_timer.h"
#include "ruvia/web/db/db.h"
#include "ruvia/web/detail/db/db_backend.h"

#include "db/db_config_storage.h"
#include "db/db_pool_operations.h"

namespace ruvia {
class redis_handle;
}

#if !defined(RUVIA_ENABLE_MARIADB) && !defined(RUVIA_ENABLE_POSTGRESQL)

#include <memory_resource>
#include <span>

#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>

namespace ruvia::detail {
class redis_registry;

class db_registry final {
public:
    db_registry(asio::io_context&, const worker_handle&, std::pmr::memory_resource*,
        std::span<const db_definition>, redis_registry* = nullptr) {}

    db_registry(const db_registry&) = delete;
    db_registry& operator=(const db_registry&) = delete;

    [[nodiscard]] task<void> connect() {
        co_return;
    }
    void close_now() noexcept {}
    [[nodiscard]] bool empty() const noexcept {
        return true;
    }
};

}  // namespace ruvia::detail

#else

#include <chrono>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <memory_resource>
#include <optional>
#include <span>
#include <string_view>
#include <variant>
#include <vector>

#include <asio/io_context.hpp>

#include "ruvia/core/memory/pmr_object.h"
#include "ruvia/core/operation_timeout.h"
#include "ruvia/core/pool_lease_scheduler.h"

#include "db/db_host_resolution.h"
#include "integration/named_capability.h"

struct st_mysql;
struct st_mysql_res;
struct pg_conn;
struct pg_result;

namespace ruvia::detail {

class redis_registry;
class db_query_cache_state;
struct db_slot_socket;
struct db_slot_socket_quarantine;

#ifdef RUVIA_ENABLE_MARIADB

class mariadb_pool final {
public:
    mariadb_pool(asio::io_context& io_context, const worker_handle& worker_value, db_config_storage config,
        std::pmr::memory_resource* resource = nullptr);
    mariadb_pool(asio::io_context&, worker_handle&&, db_config_storage,
        std::pmr::memory_resource* = nullptr) = delete;
    ~mariadb_pool();

    mariadb_pool(const mariadb_pool&) = delete;
    mariadb_pool& operator=(const mariadb_pool&) = delete;

    [[nodiscard]] task<void> connect() {
        return lifecycle_.connect();
    }
    void close_now() noexcept {
        lifecycle_.close_now();
    }
    [[nodiscard]] task<std::size_t> acquire_slot(operation_timeout timeout, stop_token token) {
        return lifecycle_.acquire_slot(timeout, std::move(token));
    }
    void release_slot(std::size_t slot) noexcept {
        lifecycle_.release_slot(slot);
    }
    void cancel_operation_by_id(std::uint64_t id) noexcept {
        lifecycle_.cancel_operation_by_id(id);
    }
    template <typename slot_type>
    void throw_if_cancelled(const slot_type& slot) const {
        lifecycle_.throw_if_cancelled(slot);
    }

    template <typename pool_type>
    friend task<void> finish_db_transaction(
        pool_type&, std::size_t, std::string_view, std::pmr::memory_resource*, const operation_options&);
    template <typename pool_type>
    friend task<db_transaction> begin_db_transaction(
        pool_type&, std::pmr::memory_resource*, operation_options,
        db_transaction_start_plan);
    template <typename pool_type>
    friend task<db_rows> execute_db_query(pool_type&, std::pmr::string, std::pmr::vector<db_value>,
        std::pmr::memory_resource*, operation_options);
    template <typename pool_type>
    friend task<db_exec_result> execute_db_command(pool_type&, std::pmr::string, std::pmr::vector<db_value>,
        std::pmr::memory_resource*, operation_options);
    template <typename pool_type, typename slot_type>
    friend task<db_resolved_addresses_type> resolve_db_host(
        pool_type&, slot_type&, ruvia::operation_timeout, std::string_view);
    template <typename pool_type>
    friend task<db_rows> query_on_db_transaction_slot(pool_type&, std::size_t, std::pmr::string,
        std::pmr::vector<db_value>, std::pmr::memory_resource*, const operation_options&);
    template <typename pool_type>
    friend task<db_exec_result> execute_on_db_transaction_slot(pool_type&, std::size_t, std::pmr::string,
        std::pmr::vector<db_value>, std::pmr::memory_resource*, const operation_options&);
    template <typename pool_type>
    friend task<std::size_t> acquire_db_slot(pool_type&, ruvia::operation_timeout, stop_token);
    template <typename pool_type>
    friend void release_db_slot(pool_type&, std::size_t) noexcept;
    template <typename pool_type>
    friend class db_slot_cancellation_guard;
    friend class db_pool_lifecycle<mariadb_pool>;
    friend class worker_cancellation_target<mariadb_pool>;
    friend class ::ruvia::db_handle;
    friend class ::ruvia::db_transaction;
    friend class ::ruvia::db_stream_result;

    using slot_guard_type = db_slot_guard<mariadb_pool>;

    struct connection_slot_type {
        connection_slot_type(asio::io_context& io_context, std::pmr::memory_resource* resource = nullptr);
        ~connection_slot_type();
        connection_slot_type(connection_slot_type&&) noexcept;
        connection_slot_type& operator=(connection_slot_type&&) noexcept;

        using slot_socket_deleter_type = pmr_object_deleter<db_slot_socket>;
        using slot_socket_quarantine_deleter_type = pmr_object_deleter<db_slot_socket_quarantine>;
        using deadline_timer_deleter_type = pmr_object_deleter<worker_timer_registration>;

        asio::ip::tcp::resolver resolver_;
        st_mysql* connection_{nullptr};
        std::unique_ptr<db_slot_socket, slot_socket_deleter_type> wait_socket_;
        std::unique_ptr<db_slot_socket_quarantine, slot_socket_quarantine_deleter_type> socket_quarantine_;
        std::unique_ptr<worker_timer_registration, deadline_timer_deleter_type> deadline_timer_;
        std::coroutine_handle<> deadline_continuation_{};
        bool connected_{false};
        // Shutdown may request closure while an async descriptor wait still
        // borrows both this slot and wait_socket. Keep the transport owner alive
        // until that wait resumes and the driving coroutine performs the final
        // close; destroying it immediately would leave the queued Asio handler
        // with dangling references.
        bool wait_active_{false};
        bool close_requested_{false};
        db_slot_abort_reason abort_reason_{db_slot_abort_reason::none};
        std::uint64_t cancellation_id_{0};
        enum class deadline_kind_type : std::uint8_t { resolve,
            socket,
            sleep };
        operation_deadline<deadline_kind_type> deadline_;

        static void expire_deadline(connection_slot_type& slot, deadline_kind_type kind) noexcept;
    };

    // Backend dispatch entry points. The class itself remains detail-only.
    void close_slot(connection_slot_type& slot) noexcept;
    task<db_resolved_addresses_type> resolve_host(connection_slot_type& slot, const ruvia::operation_timeout& deadline);
    task<void> connect_unlocked(connection_slot_type& slot, const ruvia::operation_timeout& operation_timeout);
    task<int> wait_for_mysql(connection_slot_type& slot, int status, const ruvia::operation_timeout& deadline);
    task<ruvia::operation_timeout> run_mysql_statement(connection_slot_type& slot, std::string_view sql,
        std::span<const db_value> params, std::pmr::memory_resource* resource,
        const ruvia::operation_timeout& operation_timeout);
    task<st_mysql_res*> store_mysql_result(connection_slot_type& slot, const ruvia::operation_timeout& deadline);
    task<void> finish_mysql_results(connection_slot_type& slot, const ruvia::operation_timeout& deadline);
    task<db_rows> query_on_slot(connection_slot_type& slot, std::string_view sql,
        std::span<const db_value> params, std::pmr::memory_resource* resource,
        const ruvia::operation_timeout& operation_timeout);
    task<db_exec_result> execute_on_slot(connection_slot_type& slot, std::string_view sql,
        std::span<const db_value> params, std::pmr::memory_resource* resource,
        const ruvia::operation_timeout& operation_timeout);
    task<void> execute_control(connection_slot_type& slot, std::string_view sql,
        std::pmr::memory_resource* resource, const ruvia::operation_timeout& operation_timeout);
    task<db_rows> query(std::pmr::string sql, std::pmr::vector<db_value> params,
        std::pmr::memory_resource* resource, operation_options options);
    task<db_exec_result> execute(std::pmr::string sql, std::pmr::vector<db_value> params,
        std::pmr::memory_resource* resource, operation_options options);
    task<db_stream_result> stream(std::pmr::string sql, std::pmr::vector<db_value> params,
        std::pmr::memory_resource* resource, operation_options options);
    task<std::optional<db_row>> read_stream_row(std::size_t slot, void* result,
        std::pmr::memory_resource* resource, const operation_options& options);
    task<void> close_stream(std::size_t slot, void* result, std::pmr::memory_resource* resource,
        const operation_options& options);
    void abort_stream(std::size_t slot, void* result) noexcept;
    task<db_rows> query_on_transaction_slot(std::size_t slot, std::pmr::string sql,
        std::pmr::vector<db_value> params, std::pmr::memory_resource* resource,
        const operation_options& options);
    task<db_exec_result> execute_on_transaction_slot(std::size_t slot, std::pmr::string sql,
        std::pmr::vector<db_value> params, std::pmr::memory_resource* resource,
        const operation_options& options);
    task<db_transaction> begin_transaction(
        std::pmr::memory_resource* resource, operation_options operation_options,
        db_transaction_options transaction_options);
    task<void> commit_transaction(
        std::size_t slot, std::pmr::memory_resource* resource, const operation_options& options);
    task<void> rollback_transaction(
        std::size_t slot, std::pmr::memory_resource* resource, const operation_options& options);
    void abort_transaction(std::size_t slot) noexcept;

private:
    asio::io_context& io_context_;
    db_config_storage config_;
    std::pmr::memory_resource* resource_;
    const worker_handle& worker_;
    std::pmr::vector<connection_slot_type> slots_;
    pool_lease_scheduler scheduler_;
    std::shared_ptr<db_cancellation_target<mariadb_pool>> cancellation_target_;
    db_pool_lifecycle<mariadb_pool> lifecycle_{*this};
};

#endif  // RUVIA_ENABLE_MARIADB

#ifdef RUVIA_ENABLE_POSTGRESQL

class postgresql_pool final {
public:
    postgresql_pool(asio::io_context& io_context, const worker_handle& worker_value, db_config_storage config,
        std::pmr::memory_resource* resource = nullptr);
    postgresql_pool(asio::io_context&, worker_handle&&, db_config_storage,
        std::pmr::memory_resource* = nullptr) = delete;
    ~postgresql_pool();

    postgresql_pool(const postgresql_pool&) = delete;
    postgresql_pool& operator=(const postgresql_pool&) = delete;

    [[nodiscard]] task<void> connect() {
        return lifecycle_.connect();
    }
    void close_now() noexcept {
        lifecycle_.close_now();
    }
    [[nodiscard]] task<std::size_t> acquire_slot(operation_timeout timeout, stop_token token) {
        return lifecycle_.acquire_slot(timeout, std::move(token));
    }
    void release_slot(std::size_t slot) noexcept {
        lifecycle_.release_slot(slot);
    }
    void cancel_operation_by_id(std::uint64_t id) noexcept {
        lifecycle_.cancel_operation_by_id(id);
    }
    template <typename slot_type>
    void throw_if_cancelled(const slot_type& slot) const {
        lifecycle_.throw_if_cancelled(slot);
    }

private:
    template <typename pool_type>
    friend task<void> finish_db_transaction(
        pool_type&, std::size_t, std::string_view, std::pmr::memory_resource*, const operation_options&);
    template <typename pool_type>
    friend task<db_transaction> begin_db_transaction(
        pool_type&, std::pmr::memory_resource*, operation_options,
        db_transaction_start_plan);
    template <typename pool_type>
    friend task<db_rows> execute_db_query(pool_type&, std::pmr::string, std::pmr::vector<db_value>,
        std::pmr::memory_resource*, operation_options);
    template <typename pool_type>
    friend task<db_exec_result> execute_db_command(pool_type&, std::pmr::string, std::pmr::vector<db_value>,
        std::pmr::memory_resource*, operation_options);
    template <typename pool_type, typename slot_type>
    friend task<db_resolved_addresses_type> resolve_db_host(
        pool_type&, slot_type&, ruvia::operation_timeout, std::string_view);
    template <typename pool_type>
    friend task<db_rows> query_on_db_transaction_slot(pool_type&, std::size_t, std::pmr::string,
        std::pmr::vector<db_value>, std::pmr::memory_resource*, const operation_options&);
    template <typename pool_type>
    friend task<db_exec_result> execute_on_db_transaction_slot(pool_type&, std::size_t, std::pmr::string,
        std::pmr::vector<db_value>, std::pmr::memory_resource*, const operation_options&);
    template <typename pool_type>
    friend task<std::size_t> acquire_db_slot(pool_type&, ruvia::operation_timeout, stop_token);
    template <typename pool_type>
    friend void release_db_slot(pool_type&, std::size_t) noexcept;
    template <typename pool_type>
    friend class db_slot_cancellation_guard;
    friend class db_pool_lifecycle<postgresql_pool>;
    friend class worker_cancellation_target<postgresql_pool>;
    friend class ::ruvia::db_handle;
    friend class ::ruvia::db_transaction;
    friend class ::ruvia::db_stream_result;

    using slot_guard_type = db_slot_guard<postgresql_pool>;

    struct connection_slot_type {
        connection_slot_type(asio::io_context& io_context, std::pmr::memory_resource* resource = nullptr);
        ~connection_slot_type();
        connection_slot_type(connection_slot_type&&) noexcept;
        connection_slot_type& operator=(connection_slot_type&&) noexcept;

        using slot_socket_deleter_type = pmr_object_deleter<db_slot_socket>;
        using slot_socket_quarantine_deleter_type = pmr_object_deleter<db_slot_socket_quarantine>;
        using deadline_timer_deleter_type = pmr_object_deleter<worker_timer_registration>;

        asio::ip::tcp::resolver resolver_;
        pg_conn* connection_{nullptr};
        std::unique_ptr<db_slot_socket, slot_socket_deleter_type> wait_socket_;
        std::unique_ptr<db_slot_socket_quarantine, slot_socket_quarantine_deleter_type> socket_quarantine_;
        std::unique_ptr<worker_timer_registration, deadline_timer_deleter_type> deadline_timer_;
        bool connected_{false};
        bool wait_active_{false};
        bool close_requested_{false};
        db_slot_abort_reason abort_reason_{db_slot_abort_reason::none};
        std::uint64_t cancellation_id_{0};
        enum class deadline_kind_type : std::uint8_t { resolve,
            socket };
        operation_deadline<deadline_kind_type> deadline_;

        static void expire_deadline(connection_slot_type& slot, deadline_kind_type kind) noexcept;
    };

public:
    // Backend dispatch entry points. The class itself remains detail-only.
    void close_slot(connection_slot_type& slot) noexcept;
    task<db_resolved_addresses_type> resolve_host(connection_slot_type& slot, const ruvia::operation_timeout& deadline);
    task<void> connect_unlocked(connection_slot_type& slot, const ruvia::operation_timeout& operation_timeout);
    enum class postgresql_wait_type : std::uint8_t { read,
        write,
        read_or_write };
    task<void> wait_for_postgresql(
        connection_slot_type& slot, postgresql_wait_type wait, const ruvia::operation_timeout& deadline);
    task<void> flush_output(connection_slot_type& slot, const ruvia::operation_timeout& deadline);
    task<void> wait_until_result_ready(connection_slot_type& slot, const ruvia::operation_timeout& deadline);
    task<void> send_query(connection_slot_type& slot, const std::pmr::string& sql,
        std::span<const db_value> params, const ruvia::operation_timeout& deadline, bool single_row);
    task<db_rows> query_on_slot(connection_slot_type& slot, const std::pmr::string& sql,
        std::span<const db_value> params, std::pmr::memory_resource* resource,
        const ruvia::operation_timeout& operation_timeout);
    task<db_exec_result> execute_on_slot(connection_slot_type& slot, const std::pmr::string& sql,
        std::span<const db_value> params, std::pmr::memory_resource* resource,
        const ruvia::operation_timeout& operation_timeout);
    task<void> execute_control(connection_slot_type& slot, std::string_view sql,
        std::pmr::memory_resource* resource, const ruvia::operation_timeout& operation_timeout);
    task<db_rows> query(std::pmr::string sql, std::pmr::vector<db_value> params,
        std::pmr::memory_resource* resource, operation_options options);
    task<db_exec_result> execute(std::pmr::string sql, std::pmr::vector<db_value> params,
        std::pmr::memory_resource* resource, operation_options options);
    task<db_stream_result> stream(std::pmr::string sql, std::pmr::vector<db_value> params,
        std::pmr::memory_resource* resource, operation_options options);
    task<std::optional<db_row>> read_stream_row(std::size_t slot, void* result,
        std::pmr::memory_resource* resource, const operation_options& options);
    task<void> close_stream(std::size_t slot, void* result, std::pmr::memory_resource* resource,
        const operation_options& options);
    void abort_stream(std::size_t slot, void* result) noexcept;
    task<db_rows> query_on_transaction_slot(std::size_t slot, std::pmr::string sql,
        std::pmr::vector<db_value> params, std::pmr::memory_resource* resource,
        const operation_options& options);
    task<db_exec_result> execute_on_transaction_slot(std::size_t slot, std::pmr::string sql,
        std::pmr::vector<db_value> params, std::pmr::memory_resource* resource,
        const operation_options& options);
    task<db_transaction> begin_transaction(
        std::pmr::memory_resource* resource, operation_options operation_options,
        db_transaction_options transaction_options);
    task<void> commit_transaction(
        std::size_t slot, std::pmr::memory_resource* resource, const operation_options& options);
    task<void> rollback_transaction(
        std::size_t slot, std::pmr::memory_resource* resource, const operation_options& options);
    void abort_transaction(std::size_t slot) noexcept;

private:
    asio::io_context& io_context_;
    db_config_storage config_;
    std::pmr::memory_resource* resource_;
    const worker_handle& worker_;
    std::pmr::vector<connection_slot_type> slots_;
    pool_lease_scheduler scheduler_;
    std::shared_ptr<db_cancellation_target<postgresql_pool>> cancellation_target_;
    db_pool_lifecycle<postgresql_pool> lifecycle_{*this};
};

#endif  // RUVIA_ENABLE_POSTGRESQL

class db_registry final {
public:
    db_registry(asio::io_context& io_context, const worker_handle& worker_value,
        std::pmr::memory_resource* resource, const db_config& default_config);
    db_registry(asio::io_context& io_context, const worker_handle& worker_value,
        std::pmr::memory_resource* resource, std::span<const db_definition> databases, redis_registry* redis = nullptr);
    db_registry(asio::io_context& io_context, const worker_handle& worker_value,
        std::pmr::memory_resource* resource, const db_config& config,
        const redis_handle& redis, const db_cache_config& policy);
    ~db_registry();

    db_registry(const db_registry&) = delete;
    db_registry& operator=(const db_registry&) = delete;

    task<void> connect();
    void close_now() noexcept;
    [[nodiscard]] bool empty() const noexcept;
    [[nodiscard]] db_handle get(::ruvia::operation_scope& operation_scope) const;
    [[nodiscard]] db_handle get(std::string_view alias, ::ruvia::operation_scope& operation_scope) const;

#ifdef RUVIA_ENABLE_MARIADB
    using mariadb_pool_owner_type = std::unique_ptr<mariadb_pool, pmr_object_deleter<mariadb_pool>>;
#endif
#ifdef RUVIA_ENABLE_POSTGRESQL
    using postgresql_pool_owner_type = std::unique_ptr<postgresql_pool, pmr_object_deleter<postgresql_pool>>;
#endif

#if defined(RUVIA_ENABLE_MARIADB) && defined(RUVIA_ENABLE_POSTGRESQL)
    using pool_owner_type = std::variant<std::monostate, mariadb_pool_owner_type, postgresql_pool_owner_type>;
#elif defined(RUVIA_ENABLE_MARIADB)
    using pool_owner_type = std::variant<std::monostate, mariadb_pool_owner_type>;
#else
    using pool_owner_type = std::variant<std::monostate, postgresql_pool_owner_type>;
#endif

private:
    void add(asio::io_context& io_context, const worker_handle& worker, db_config_storage config);

    void attach_cache(std::size_t index, const worker_handle& worker, const redis_handle& redis,
        const db_cache_config_storage& policy, const db_config_storage& config, std::string_view alias);
    std::pmr::memory_resource* resource_;
    ::ruvia::operation_scope cache_scope_;
    struct entry_type final {
        pool_owner_type pool_;
        std::unique_ptr<db_query_cache_state, pmr_object_deleter<db_query_cache_state>> cache_;
    };
    std::pmr::vector<entry_type> entries_;
    named_capability_index alias_index_;
};

}  // namespace ruvia::detail

#endif
