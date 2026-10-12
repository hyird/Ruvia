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
#include <cstddef>
#include <cstdint>
#include <memory>
#include <memory_resource>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include <asio/io_context.hpp>
#include <asio/ssl/context.hpp>

#include "ruvia/core/memory/pmr_object.h"
#include "ruvia/core/operation_timeout.h"

#include "client/client_tcp_transport.h"
#include "integration/slot_pool.h"
#include "redis/redis_client_runtime.h"

struct redisReader;

namespace ruvia::detail {

struct redis_reader_deleter {
    void operator()(redisReader* reader) const noexcept;
};

class redis_reply_builder;

struct redis_command_args_view final {
    std::span<const std::pmr::string> args_;
};

inline constexpr std::size_t redis_read_buffer_bytes = 8192;
// Serialized request storage a pooled connection keeps after a write: this
// much, or four times the request it just sent when larger. A steady stream
// of similar-sized commands reuses one allocation while a single oversized
// request is not pinned for the connection's lifetime.
inline constexpr std::size_t redis_retained_write_buffer_bytes = std::size_t{64} * 1024;

class redis_pool final {
public:
    // `tls_context` is borrowed from the owning runtime and shared by every
    // pool of the alias; it is configured only when TLS is enabled.
    redis_pool(asio::io_context& io_context, const redis_config_storage& config, asio::ssl::context& tls_context,
        std::optional<std::chrono::milliseconds> command_timeout, std::size_t pool_size,
        const worker_handle& worker_value, std::pmr::memory_resource* resource = nullptr);
    redis_pool(asio::io_context&, redis_config_storage&&, asio::ssl::context&, std::optional<std::chrono::milliseconds>,
        std::size_t, const worker_handle&, std::pmr::memory_resource* = nullptr) = delete;
    redis_pool(asio::io_context&, const redis_config_storage&&, asio::ssl::context&,
        std::optional<std::chrono::milliseconds>, std::size_t, const worker_handle&,
        std::pmr::memory_resource* = nullptr) = delete;
    redis_pool(asio::io_context&, const redis_config_storage&, asio::ssl::context&,
        std::optional<std::chrono::milliseconds>, std::size_t, worker_handle&&,
        std::pmr::memory_resource* = nullptr) = delete;
    redis_pool(asio::io_context&, const redis_config_storage&, asio::ssl::context&,
        std::optional<std::chrono::milliseconds>, std::size_t, const worker_handle&&,
        std::pmr::memory_resource* = nullptr) = delete;
    ~redis_pool();

    redis_pool(const redis_pool&) = delete;
    redis_pool& operator=(const redis_pool&) = delete;

    task<void> connect();
    void close_now() noexcept;
    // `reply_timeout` starts when the command is written, so a server-side
    // block window is not consumed by slot acquisition or a lazy connect.
    task<redis_value> execute_owned(std::pmr::vector<std::pmr::string> args,
        std::pmr::memory_resource* resource, operation_options options = {},
        std::optional<std::chrono::milliseconds> reply_timeout = std::nullopt);
    // The caller keeps `args` alive until the operation completes.
    task<redis_value> execute_borrowed(redis_command_args_view args, std::pmr::memory_resource* resource,
        operation_options options);
    task<std::pmr::vector<redis_value>> execute_pipeline(
        std::span<const redis_owned_command> commands, operation_options options,
        std::pmr::memory_resource* resource);
    task<std::pmr::vector<redis_value>> execute_pipeline(
        std::span<const redis_command_args_view> commands, operation_options options,
        std::pmr::memory_resource* resource);

private:
    friend class ::ruvia::redis_handle;

    // Every deadline belongs to a connection's transport; the slot pool only
    // leases slots and routes cancellation.
    struct slot_policy final {
        using owner_type = redis_pool;
        enum class deadline_kind_type : std::uint8_t { none };

        [[noreturn]] static void throw_acquire_failure(const redis_pool& pool, const pool_waiter_result& result);
        [[noreturn]] static void throw_aborted(const redis_pool& pool, pool_slot_abort_reason reason);
        static void expire_slot(redis_pool&, std::size_t, deadline_kind_type) noexcept {}
        static void cancel_slot(redis_pool& pool, std::size_t slot) noexcept;
    };
    using slot_pool_type = slot_pool<slot_policy>;

    // One pooled Redis connection: the shared outbound transport plus the
    // RESP request and reply buffers. Address-stable: the transport's pending
    // I/O and timers and the reader's object functions borrow it.
    struct connection_type final {
        connection_type(redis_pool& pool, client_tcp_endpoint endpoint);
        ~connection_type();

        connection_type(const connection_type&) = delete;
        connection_type& operator=(const connection_type&) = delete;

        client_tcp_transport transport_;
        std::pmr::string write_buffer_;
        std::array<char, redis_read_buffer_bytes> read_buffer_;
        // Created on first connect; the reader's object functions address the
        // builder, so it outlives every reader.
        std::unique_ptr<redis_reply_builder, pmr_object_deleter<redis_reply_builder>> reply_builder_;
        std::unique_ptr<redisReader, redis_reader_deleter> reader_;
        std::size_t reply_bytes_{0};
        bool connected_{false};
    };
    using connection_owner_type = std::unique_ptr<connection_type, pmr_object_deleter<connection_type>>;

    void close(std::size_t slot) noexcept;
    // Cancellation/shutdown: records the reason so a resolve completing after
    // this point starts no connect or handshake, then closes the connection.
    void abort(std::size_t slot, client_abort_reason reason) noexcept;
    void ensure_reader(connection_type& connection);
    task<void> connect(std::size_t slot, const ruvia::operation_timeout* operation_timeout = nullptr);
    task<void> authenticate(std::size_t slot, const ruvia::operation_timeout& connect_timeout);
    // Sends and then releases the connection's serialized request.
    task<void> write_request(std::size_t slot, const ruvia::operation_timeout& timeout);
    task<redis_value> read_reply(std::size_t slot, const ruvia::operation_timeout& timeout,
        std::pmr::memory_resource* resource);
    template <typename arg_source_type>
    task<redis_value> execute_with_timeout_impl(arg_source_type args, operation_options options,
        std::optional<std::chrono::milliseconds> reply_timeout, std::pmr::memory_resource* resource);
    template <typename command_source_type>
    task<std::pmr::vector<redis_value>> execute_pipeline_impl(
        command_source_type commands, operation_options options, std::pmr::memory_resource* resource);

    asio::io_context& io_context_;
    const redis_config_storage& config_;
    asio::ssl::context& tls_context_;
    std::optional<std::chrono::milliseconds> command_timeout_;
    std::pmr::memory_resource* resource_;
    client_wire_counters wire_counters_;
    slot_pool_type slots_;
    // Declared after the slot pool, whose worker every transport borrows.
    std::pmr::vector<connection_owner_type> connections_;
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
