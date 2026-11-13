#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <asio/io_context.hpp>
#include <asio/ssl/context.hpp>

#include "ruvia/core/operation_deadline.h"
#include "ruvia/core/operation_options.h"
#include "ruvia/core/task.h"
#include "ruvia/core/worker_ref.h"
#include "ruvia/web/redis/redis_session.h"
#include "ruvia/web/redis/redis_types.h"

#include "client/client_tcp_transport.h"
#include "integration/slot_pool.h"
#include "redis/redis_config_storage.h"

struct redisReader;

namespace ruvia::detail {

struct redis_reader_deleter {
    void operator()(redisReader* reader) const noexcept;
};

class redis_reply_builder;

inline constexpr std::size_t redis_read_buffer_bytes = 8192;
// Serialized request capacity retained after a write's transport borrow ends.
// Larger buffers return to their PMR resource, including on failure.
inline constexpr std::size_t redis_retained_write_buffer_bytes = std::size_t{64} * 1024;

// A credential request (AUTH) is wiped from the write buffer once written, so
// the retained buffer never holds a password after the write completes.
enum class redis_request_content : bool { ordinary,
    credential };

// One Redis connection: the shared outbound transport plus the RESP request
// and reply buffers. Pool slots and subscriptions each own one. Address-stable:
// the transport's pending I/O and timers and the reader's object functions
// borrow it. The owner serializes requests into write_buffer(), sends them
// with write_request() and reads replies one at a time; a subscription keeps
// reading pushed replies without writing.
//
// Every failed transport result is mapped to redis_error here. An owner
// abort (cancelled or closing) is recorded on the transport and checked
// after each wait, so cancellation and shutdown never surface as I/O errors;
// an aborted result without an owner reason is an expired deadline.
class redis_connection final {
public:
    redis_connection(asio::io_context& io_context, asio::ssl::context& tls_context, const worker_ref& worker_value,
        const redis_config_storage& config, client_wire_counters& counters);
    redis_connection(asio::io_context&, asio::ssl::context&, worker_ref&&, const redis_config_storage&,
        client_wire_counters&) = delete;
    ~redis_connection();

    redis_connection(const redis_connection&) = delete;
    redis_connection& operator=(const redis_connection&) = delete;

    [[nodiscard]] bool connected() const noexcept {
        return connected_;
    }

    // Connects when closed. A connected connection whose peer closed it or
    // sent unsolicited bytes would desynchronize the next reply, so it is
    // replaced. The optional operation deadline bounds establishment together
    // with connect_timeout; AUTH and SELECT replies also honor command_timeout.
    task<void> connect(const operation_deadline* operation_timeout_value,
        std::optional<timeout_duration> command_timeout);
    // Serialized request storage: append RESP commands, then write_request().
    [[nodiscard]] std::string& write_buffer() noexcept {
        return write_buffer_;
    }
    // Sends and then releases the serialized request, wiping it first when it
    // carries a credential, whether or not the write succeeded.
    task<void> write_request(
        const operation_deadline& timeout, redis_request_content content = redis_request_content::ordinary);
    // Reads one complete reply, allocating its nodes from `resource`. Throws
    // redis_error(io_error) when the connection is or becomes closed. A
    // non-null `input_time` receives the time of every socket read that
    // delivered bytes, so a subscription's idle clock restarts on partial
    // replies too.
    task<redis_value> read_reply(const operation_deadline& timeout,
        std::chrono::steady_clock::time_point* input_time = nullptr);

    void close() noexcept;
    // Cancellation/shutdown: records the reason so a resolve completing after
    // this point starts no connect or handshake, then closes the connection.
    void abort(client_abort_reason reason) noexcept;
    // Throws the redis_error of a recorded owner abort (cancelled or closing).
    void throw_if_aborted() const;

private:
    void ensure_reader();
    task<void> authenticate(const operation_deadline& connect_timeout,
        std::optional<timeout_duration> command_timeout);
    // One connection-setup command (AUTH, SELECT) and its reply.
    task<redis_value> exchange_control_command(
        std::span<const std::string_view> args, operation_deadline deadline, redis_request_content content);

    const redis_config_storage& config_;
    client_tcp_transport transport_;
    std::string write_buffer_;
    std::array<char, redis_read_buffer_bytes> read_buffer_;
    // Created on first connect; the reader's object functions address the
    // builder, so it outlives every reader.
    std::unique_ptr<redis_reply_builder> reply_builder_;
    std::unique_ptr<redisReader, redis_reader_deleter> reader_;
    std::size_t reply_bytes_{0};
    bool connected_{false};
};

}  // namespace ruvia::detail

namespace ruvia::detail {

struct redis_command_args_view final {
    std::span<const std::string> args_;
};

class redis_pool final {
public:
    // `tls_context` is borrowed from the owning runtime and shared by every
    // pool of the alias; it is configured only when TLS is enabled.
    redis_pool(asio::io_context& io_context, const redis_config_storage& config, asio::ssl::context& tls_context,
        std::optional<timeout_duration> command_timeout, std::size_t pool_size,
        const worker_ref& worker_value);
    redis_pool(asio::io_context&, redis_config_storage&&, asio::ssl::context&, std::optional<timeout_duration>,
        std::size_t, const worker_ref&) = delete;
    redis_pool(asio::io_context&, const redis_config_storage&&, asio::ssl::context&,
        std::optional<timeout_duration>, std::size_t, const worker_ref&) = delete;
    redis_pool(asio::io_context&, const redis_config_storage&, asio::ssl::context&,
        std::optional<timeout_duration>, std::size_t, worker_ref&&) = delete;
    redis_pool(asio::io_context&, const redis_config_storage&, asio::ssl::context&,
        std::optional<timeout_duration>, std::size_t, const worker_ref&&) = delete;
    ~redis_pool();

    redis_pool(const redis_pool&) = delete;
    redis_pool& operator=(const redis_pool&) = delete;

    task<void> connect();
    void close_now() noexcept;
    // `reply_timeout` starts when the command is written, so a server-side
    // block window is not consumed by slot acquisition or a lazy connect.
    // `io_metrics` is the request's counter block, or null outside a request.
    task<redis_value> execute_owned(redis_owned_command command, operation_options options = {},
        std::optional<timeout_duration> reply_timeout = std::nullopt,
        request_io_metrics* io_metrics = nullptr, std::optional<operation_deadline> deadline = std::nullopt);
    task<redis_value> execute_owned(std::vector<std::string> args, operation_options options = {},
        std::optional<timeout_duration> reply_timeout = std::nullopt,
        request_io_metrics* io_metrics = nullptr, std::optional<operation_deadline> deadline = std::nullopt);
    // The caller keeps `args` alive until the operation completes. `deadline`
    // is copied, so repeated commands share the operation's absolute bound.
    task<redis_value> execute_borrowed(redis_command_args_view args,
        operation_options options, operation_deadline deadline);
    task<std::vector<redis_value>> execute_pipeline(
        std::vector<redis_owned_command> commands, operation_options options, bool wrap_transaction = false);

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
    using connection_owner_type = std::unique_ptr<redis_connection>;

    template <typename result_type, typename command_source_type>
    task<result_type> execute_commands(command_source_type source, operation_options options,
        std::optional<timeout_duration> reply_timeout,
        std::optional<operation_deadline> deadline, request_io_metrics* io_metrics = nullptr,
        bool wrap_transaction = false);

    const redis_config_storage& config_;
    std::optional<timeout_duration> command_timeout_;
    client_wire_counters wire_counters_;
    slot_pool_type slots_;
    // Declared after the slot pool, whose worker every transport borrows.
    std::vector<connection_owner_type> connections_;
};

}  // namespace ruvia::detail
