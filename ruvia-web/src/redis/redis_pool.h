#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <memory_resource>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <asio/io_context.hpp>
#include <asio/ssl/context.hpp>

#include "ruvia/core/memory/pmr_object.h"
#include "ruvia/core/operation_options.h"
#include "ruvia/core/operation_timeout.h"
#include "ruvia/core/worker_handle.h"
#include "ruvia/web/detail/redis/redis_owned_command.h"
#include "ruvia/web/redis/redis_handle.h"

#include "integration/slot_pool.h"
#include "redis/redis_config_storage.h"
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
    // `io_metrics` is the request's counter block, or null outside a request.
    task<redis_value> execute_owned(redis_owned_command command,
        std::pmr::memory_resource* resource, operation_options options = {},
        std::optional<std::chrono::milliseconds> reply_timeout = std::nullopt,
        request_io_metrics* io_metrics = nullptr, std::optional<operation_timeout> deadline = std::nullopt);
    task<redis_value> execute_owned(std::pmr::vector<std::pmr::string> args,
        std::pmr::memory_resource* resource, operation_options options = {},
        std::optional<std::chrono::milliseconds> reply_timeout = std::nullopt,
        request_io_metrics* io_metrics = nullptr, std::optional<operation_timeout> deadline = std::nullopt);
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
        std::optional<operation_timeout> deadline, request_io_metrics* io_metrics = nullptr,
        bool wrap_transaction = false);

    const redis_config_storage& config_;
    std::optional<std::chrono::milliseconds> command_timeout_;
    std::pmr::memory_resource* resource_;
    client_wire_counters wire_counters_;
    slot_pool_type slots_;
    // Declared after the slot pool, whose worker every transport borrows.
    std::pmr::vector<connection_owner_type> connections_;
};

}  // namespace ruvia::detail
