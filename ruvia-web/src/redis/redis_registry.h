#pragma once

#include "ruvia/core/operation_deadline.h"
#include "ruvia/core/worker_cancellation.h"
#include "ruvia/core/worker_handle.h"
#include "ruvia/web/detail/redis/redis_owned_command.h"
#include "ruvia/web/redis/redis_handle.h"

#include "integration/named_capability.h"
#include "redis/redis_config_storage.h"

#ifndef RUVIA_ENABLE_REDIS

#include <memory_resource>
#include <span>
#include <stdexcept>

#include <asio/io_context.hpp>

namespace ruvia::detail {

class redis_registry final {
public:
    redis_registry(asio::io_context&, std::pmr::memory_resource*, std::span<const redis_definition_type>,
        worker_handle worker_value) {
        if (!worker_value.valid()) {
            throw std::invalid_argument("redis registry requires a valid worker");
        }
    }

    redis_registry(const redis_registry&) = delete;
    redis_registry& operator=(const redis_registry&) = delete;

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

#include <array>
#include <chrono>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <memory_resource>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/ssl/context.hpp>
#include <asio/ssl/stream.hpp>

#include "ruvia/core/memory/pmr_object.h"
#include "ruvia/core/operation_timeout.h"
#include "ruvia/core/pool_lease_scheduler.h"

#include "redis/redis_client_runtime.h"

struct redisReader;

namespace ruvia::detail {

template <typename result_type>
class asio_completion;

struct redis_reader_deleter {
    void operator()(redisReader* reader) const noexcept;
};

struct redis_reader_budget;
struct redis_reader_budget_deleter final {
    std::pmr::memory_resource* resource_{nullptr};
    void operator()(redis_reader_budget* budget) const noexcept;
};

class redis_pool;
using redis_cancellation_target = worker_cancellation_target<redis_pool>;

struct redis_command_args_view final {
    std::span<const std::pmr::string> args_;
};

inline constexpr std::size_t redis_read_buffer_bytes = 8192;

class redis_pool final {
public:
    redis_pool(asio::io_context& io_context, const redis_config_storage& config,
        std::optional<std::chrono::milliseconds> command_timeout, std::size_t pool_size,
        const worker_handle& worker_value, std::pmr::memory_resource* resource = nullptr);
    redis_pool(asio::io_context&, redis_config_storage&&, std::optional<std::chrono::milliseconds>,
        std::size_t, const worker_handle&, std::pmr::memory_resource* = nullptr) = delete;
    redis_pool(asio::io_context&, const redis_config_storage&&,
        std::optional<std::chrono::milliseconds>, std::size_t, const worker_handle&,
        std::pmr::memory_resource* = nullptr) = delete;
    redis_pool(asio::io_context&, const redis_config_storage&,
        std::optional<std::chrono::milliseconds>, std::size_t, worker_handle&&,
        std::pmr::memory_resource* = nullptr) = delete;
    redis_pool(asio::io_context&, const redis_config_storage&,
        std::optional<std::chrono::milliseconds>, std::size_t, const worker_handle&&,
        std::pmr::memory_resource* = nullptr) = delete;
    ~redis_pool();

    redis_pool(const redis_pool&) = delete;
    redis_pool& operator=(const redis_pool&) = delete;

    task<void> connect();
    void close_now() noexcept;
    task<redis_value> execute_owned(std::pmr::vector<std::pmr::string> args,
        std::pmr::memory_resource* resource, operation_options options = {});
    task<std::pmr::vector<redis_value>> execute_pipeline(
        std::span<const redis_owned_command> commands, operation_options options,
        std::pmr::memory_resource* resource);
    task<std::pmr::vector<redis_value>> execute_pipeline(
        std::span<const redis_command_args_view> commands, operation_options options,
        std::pmr::memory_resource* resource);

private:
    friend class ::ruvia::redis_handle;
    friend class worker_cancellation_target<redis_pool>;

    struct connection_type final {
        explicit connection_type(asio::io_context& io_context, std::pmr::memory_resource* resource);
        ~connection_type();

        connection_type(const connection_type&) = delete;
        connection_type& operator=(const connection_type&) = delete;
        connection_type(connection_type&&) noexcept;
        connection_type& operator=(connection_type&&) noexcept;

        asio::ip::tcp::socket socket_;
        // Pool construction reserves all slots before any TLS stream exists.
        // Once connect() starts, slots never move: TLS and pending I/O borrow
        // this socket until their owning connection is retired.
        using tls_stream_type = asio::ssl::stream<asio::ip::tcp::socket&>;
        std::unique_ptr<tls_stream_type, pmr_object_deleter<tls_stream_type>> tls_stream_;
        asio::ip::tcp::resolver resolver_;
        std::pmr::string write_buffer_;
        std::array<char, redis_read_buffer_bytes> read_buffer_;
        std::unique_ptr<redis_reader_budget, redis_reader_budget_deleter> reader_budget_;
        std::unique_ptr<redisReader, redis_reader_deleter> reader_;
        std::size_t reply_bytes_{0};
        bool connected_{false};
        enum class abort_reason_type : std::uint8_t { none,
            cancelled,
            closing };
        abort_reason_type abort_reason_{abort_reason_type::none};
        std::uint64_t cancellation_id_{0};
        enum class deadline_kind_type : std::uint8_t { resolve,
            socket };
        operation_deadline<deadline_kind_type> deadline_;
        std::unique_ptr<worker_timer_registration, pmr_object_deleter<worker_timer_registration>>
            deadline_timer_;
    };

    class connection_guard_type final {
    public:
        connection_guard_type(redis_pool& pool, std::size_t index, const stop_token& stop_token_value);
        connection_guard_type(const connection_guard_type&) = delete;
        connection_guard_type& operator=(const connection_guard_type&) = delete;
        ~connection_guard_type();

        [[nodiscard]] connection_type& connection() noexcept;
        void discard() noexcept;

    private:
        redis_pool& pool_;
        std::size_t index_{0};
        worker_cancellation_registration<redis_cancellation_target> cancellation_;
        bool discard_{false};
    };

    task<std::size_t> acquire(const ruvia::operation_timeout& timeout, stop_token stop_token);
    void release(std::size_t index) noexcept;
    void close(connection_type& connection) noexcept;
    void configure_socket(connection_type& connection) noexcept;
    void ensure_reader(connection_type& connection);
    [[nodiscard]] bool arm_deadline(
        connection_type& connection, const ruvia::operation_timeout& timeout, connection_type::deadline_kind_type kind);
    [[nodiscard]] bool clear_deadline(connection_type& connection) noexcept;
    task<void> connect(connection_type& connection, const ruvia::operation_timeout* operation_timeout = nullptr);
    task<void> authenticate(connection_type& connection, const ruvia::operation_timeout& connect_timeout);
    task<redis_value> read_reply(connection_type& connection, const ruvia::operation_timeout& timeout,
        std::pmr::memory_resource* resource);
    template <typename arg_source_type>
    task<redis_value> execute_with_timeout_impl(
        arg_source_type args, operation_options options, std::pmr::memory_resource* resource);
    template <typename command_source_type>
    task<std::pmr::vector<redis_value>> execute_pipeline_impl(
        command_source_type commands, operation_options options, std::pmr::memory_resource* resource);
    task<void> async_socket_write(connection_type& connection, const ruvia::operation_timeout& timeout);
    task<asio_completion<std::size_t>> async_socket_read_some(
        connection_type& connection, std::span<char> buffer, const ruvia::operation_timeout& timeout);
    void cancel_operation_by_id(std::uint64_t cancellation_id) noexcept;
    void throw_if_aborted(const connection_type& connection) const;
    asio::io_context& io_context_;
    const worker_handle& worker_;
    const redis_config_storage& config_;
    std::optional<std::chrono::milliseconds> command_timeout_;
    std::pmr::memory_resource* resource_;
    std::optional<asio::ssl::context> tls_context_;
    std::pmr::vector<connection_type> connections_;
    pool_lease_scheduler scheduler_;
    std::shared_ptr<redis_cancellation_target> cancellation_target_;
};

struct redis_command_executor final {
    redis_pool* pool_{nullptr};
    operation_options options_;
};

class redis_registry final {
public:
    redis_registry(asio::io_context& io_context, std::pmr::memory_resource* resource,
        std::span<const redis_definition_type> redis, worker_handle worker_value);
    ~redis_registry();

    redis_registry(const redis_registry&) = delete;
    redis_registry& operator=(const redis_registry&) = delete;

    task<void> connect();
    void close_now() noexcept;

    [[nodiscard]] bool empty() const noexcept;
    [[nodiscard]] redis_handle get(::ruvia::operation_scope& operation_scope) const;
    [[nodiscard]] redis_handle get(std::string_view alias, ::ruvia::operation_scope& operation_scope) const;

private:
    using entry_type = std::unique_ptr<redis_client_runtime, pmr_object_deleter<redis_client_runtime>>;

    worker_handle worker_;
    std::pmr::memory_resource* resource_;
    std::pmr::vector<entry_type> pools_;
    named_capability_index alias_index_;
};

}  // namespace ruvia::detail

#endif
