#pragma once

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

#include "integration/slot_pool.h"

#include "redis/redis_client_runtime.h"

struct redisReader;

namespace ruvia::detail {

template <typename result_type>
class asio_completion;

struct redis_reader_deleter {
    void operator()(redisReader* reader) const noexcept;
};

class redis_reply_builder;

struct redis_command_args_view final {
    std::span<const std::pmr::string> args_;
};

inline constexpr std::size_t redis_read_buffer_bytes = 8192;

class redis_pool final {
public:
    // `tls_context` is borrowed from the owning runtime and shared by every
    // pool of the alias; nullptr selects plaintext.
    redis_pool(asio::io_context& io_context, const redis_config_storage& config, asio::ssl::context* tls_context,
        std::optional<std::chrono::milliseconds> command_timeout, std::size_t pool_size,
        const worker_handle& worker_value, std::pmr::memory_resource* resource = nullptr);
    redis_pool(asio::io_context&, redis_config_storage&&, asio::ssl::context*, std::optional<std::chrono::milliseconds>,
        std::size_t, const worker_handle&, std::pmr::memory_resource* = nullptr) = delete;
    redis_pool(asio::io_context&, const redis_config_storage&&, asio::ssl::context*,
        std::optional<std::chrono::milliseconds>, std::size_t, const worker_handle&,
        std::pmr::memory_resource* = nullptr) = delete;
    redis_pool(asio::io_context&, const redis_config_storage&, asio::ssl::context*,
        std::optional<std::chrono::milliseconds>, std::size_t, worker_handle&&,
        std::pmr::memory_resource* = nullptr) = delete;
    redis_pool(asio::io_context&, const redis_config_storage&, asio::ssl::context*,
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

    struct slot_policy final {
        using owner_type = redis_pool;
        enum class deadline_kind_type : std::uint8_t { resolve,
            socket };

        [[noreturn]] static void throw_acquire_failure(const redis_pool& pool, const pool_waiter_result& result);
        [[noreturn]] static void throw_aborted(const redis_pool& pool, pool_slot_abort_reason reason);
        static void expire_slot(redis_pool& pool, std::size_t slot, deadline_kind_type kind) noexcept;
        static void cancel_slot(redis_pool& pool, std::size_t slot) noexcept;
    };
    using slot_pool_type = slot_pool<slot_policy>;
    using deadline_kind_type = slot_policy::deadline_kind_type;

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
        // The reader's object functions address the builder, so it lives
        // apart from the movable connection and outlives every reader.
        std::unique_ptr<redis_reply_builder, pmr_object_deleter<redis_reply_builder>> reply_builder_;
        std::unique_ptr<redisReader, redis_reader_deleter> reader_;
        std::size_t reply_bytes_{0};
        bool connected_{false};
    };

    void close(std::size_t slot) noexcept;
    void configure_socket(connection_type& connection) noexcept;
    void ensure_reader(connection_type& connection);
    task<void> connect(std::size_t slot, const ruvia::operation_timeout* operation_timeout = nullptr);
    task<void> authenticate(std::size_t slot, const ruvia::operation_timeout& connect_timeout);
    task<redis_value> read_reply(std::size_t slot, const ruvia::operation_timeout& timeout,
        std::pmr::memory_resource* resource);
    template <typename arg_source_type>
    task<redis_value> execute_with_timeout_impl(
        arg_source_type args, operation_options options, std::pmr::memory_resource* resource);
    template <typename command_source_type>
    task<std::pmr::vector<redis_value>> execute_pipeline_impl(
        command_source_type commands, operation_options options, std::pmr::memory_resource* resource);
    task<void> async_socket_write(std::size_t slot, const ruvia::operation_timeout& timeout);
    task<asio_completion<std::size_t>> async_socket_read_some(
        std::size_t slot, std::span<char> buffer, const ruvia::operation_timeout& timeout);

    asio::io_context& io_context_;
    const redis_config_storage& config_;
    asio::ssl::context* tls_context_;
    std::optional<std::chrono::milliseconds> command_timeout_;
    std::pmr::memory_resource* resource_;
    std::pmr::vector<connection_type> connections_;
    slot_pool_type slots_;
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
