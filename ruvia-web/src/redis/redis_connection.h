#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <memory>
#include <memory_resource>
#include <optional>
#include <string>

#include <asio/io_context.hpp>
#include <asio/ssl/context.hpp>

#include "ruvia/core/memory/pmr_object.h"
#include "ruvia/core/operation_timeout.h"
#include "ruvia/core/task.h"
#include "ruvia/core/worker_handle.h"
#include "ruvia/web/redis/redis_types.h"

#include "client/client_tcp_transport.h"
#include "redis/redis_config_storage.h"

struct redisReader;

namespace ruvia::detail {

struct redis_reader_deleter {
    void operator()(redisReader* reader) const noexcept;
};

class redis_reply_builder;

inline constexpr std::size_t redis_read_buffer_bytes = 8192;
// Serialized request storage a connection keeps after a write: this much, or
// four times the request it just sent when larger. A steady stream of
// similar-sized commands reuses one allocation while a single oversized
// request is not pinned for the connection's lifetime.
inline constexpr std::size_t redis_retained_write_buffer_bytes = std::size_t{64} * 1024;

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
    redis_connection(asio::io_context& io_context, asio::ssl::context& tls_context, const worker_handle& worker_value,
        const redis_config_storage& config, client_wire_counters& counters, std::pmr::memory_resource* resource);
    redis_connection(asio::io_context&, asio::ssl::context&, worker_handle&&, const redis_config_storage&,
        client_wire_counters&, std::pmr::memory_resource*) = delete;
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
    task<void> connect(const operation_timeout* operation_timeout_value,
        std::optional<std::chrono::milliseconds> command_timeout);
    // Serialized request storage: append RESP commands, then write_request().
    [[nodiscard]] std::pmr::string& write_buffer() noexcept {
        return write_buffer_;
    }
    // Sends and then releases the serialized request.
    task<void> write_request(const operation_timeout& timeout);
    // Reads one complete reply, allocating its nodes from `resource`.
    task<redis_value> read_reply(const operation_timeout& timeout, std::pmr::memory_resource* resource);

    void close() noexcept;
    // Cancellation/shutdown: records the reason so a resolve completing after
    // this point starts no connect or handshake, then closes the connection.
    void abort(client_abort_reason reason) noexcept;
    // Throws the redis_error of a recorded owner abort (cancelled or closing).
    void throw_if_aborted() const;

private:
    void ensure_reader();
    task<void> authenticate(const operation_timeout& connect_timeout,
        std::optional<std::chrono::milliseconds> command_timeout);

    const redis_config_storage& config_;
    std::pmr::memory_resource* resource_;
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

}  // namespace ruvia::detail
