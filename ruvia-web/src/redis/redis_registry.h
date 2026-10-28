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

#include "integration/slot_pool.h"
#include "redis/redis_connection.h"

namespace ruvia::detail {

struct redis_command_args_view final {
    std::span<const std::pmr::string> args_;
};

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
        std::optional<std::chrono::milliseconds> reply_timeout = std::nullopt,
        operation_scope* scope = nullptr, std::optional<operation_timeout> deadline = std::nullopt);
    // The caller keeps `args` alive until the operation completes. `deadline`
    // is copied, so repeated commands share the operation's absolute bound.
    task<redis_value> execute_borrowed(redis_command_args_view args, std::pmr::memory_resource* resource,
        operation_options options, operation_timeout deadline);
    task<std::pmr::vector<redis_value>> execute_pipeline(
        std::pmr::vector<redis_owned_command> commands, operation_options options,
        std::pmr::memory_resource* resource, bool wrap_transaction = false);

private:
    // Every deadline belongs to a connection's transport; the slot pool only
    // leases slots and routes cancellation.
    struct slot_policy final {
        using owner_type = redis_pool;
        enum class deadline_kind_type : std::uint8_t { none };

        [[noreturn]] static void throw_acquire_failure(const redis_pool& pool, worker_wait_status status);
        [[noreturn]] static void throw_aborted(const redis_pool& pool, pool_slot_abort_reason reason);
        static void expire_slot(redis_pool&, std::size_t, deadline_kind_type) noexcept {}
        static void cancel_slot(redis_pool& pool, std::size_t slot) noexcept;
    };
    using slot_pool_type = slot_pool<slot_policy>;
    using connection_owner_type = std::unique_ptr<redis_connection, pmr_object_deleter<redis_connection>>;

    template <typename result_type, typename command_source_type>
    task<result_type> execute_commands(command_source_type source, operation_options options,
        std::optional<std::chrono::milliseconds> reply_timeout, std::pmr::memory_resource* resource,
        std::optional<operation_timeout> deadline, operation_scope* scope = nullptr,
        bool wrap_transaction = false);

    const redis_config_storage& config_;
    std::optional<std::chrono::milliseconds> command_timeout_;
    std::pmr::memory_resource* resource_;
    client_wire_counters wire_counters_;
    slot_pool_type slots_;
    // Declared after the slot pool, whose worker every transport borrows.
    std::pmr::vector<connection_owner_type> connections_;
};

struct redis_command_executor final {
    redis_pool* pool_{nullptr};
    operation_scope* scope_{nullptr};
    operation_options options_;

    [[nodiscard]] task<redis_value> execute(std::pmr::vector<std::pmr::string> args,
        std::pmr::memory_resource* resource,
        std::optional<std::chrono::milliseconds> reply_timeout = std::nullopt);
};

}  // namespace ruvia::detail

#include "redis/redis_client_runtime.h"

namespace ruvia::detail {

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
