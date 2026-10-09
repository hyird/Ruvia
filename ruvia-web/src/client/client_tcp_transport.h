#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <memory_resource>
#include <span>
#include <string_view>
#include <system_error>

#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/ssl/stream.hpp>

#include "ruvia/core/memory/pmr_object.h"
#include "ruvia/core/operation_deadline.h"
#include "ruvia/core/operation_timeout.h"
#include "ruvia/core/task.h"
#include "ruvia/core/worker_handle.h"
#include "ruvia/core/worker_timer.h"
#include "ruvia/web/http_client_types.h"

namespace ruvia::detail {

struct http_client_config_storage;
class http_client_response_state;

enum class client_abort_reason : std::uint8_t { none,
    timeout,
    cancelled,
    closing };
enum class client_deadline_kind : std::uint8_t { resolve,
    socket,
    response_buffer };

struct client_wire_counters final {
    std::size_t sent_{};
    std::size_t received_{};
};

// One worker owns the socket, resolver, deadline and borrowed response waiter.
// Origin configuration and counters outlive every transport in the pool.
class client_tcp_transport final {
public:
    client_tcp_transport(asio::io_context& io, asio::ssl::context& tls,
        const worker_handle& worker_value, const http_client_config_storage& config,
        client_wire_counters& counters, std::pmr::memory_resource* resource);
    ~client_tcp_transport();
    client_tcp_transport(const client_tcp_transport&) = delete;
    client_tcp_transport& operator=(const client_tcp_transport&) = delete;
    client_tcp_transport(client_tcp_transport&&) noexcept;
    client_tcp_transport& operator=(client_tcp_transport&&) = delete;

    [[nodiscard]] asio::ip::tcp::resolver& resolver() noexcept {
        return resolver_;
    }
    [[nodiscard]] asio::ssl::stream<asio::ip::tcp::socket>& stream() noexcept {
        return stream_;
    }
    [[nodiscard]] client_abort_reason abort_reason() const noexcept {
        return abort_reason_;
    }
    void reset_abort() noexcept {
        abort_reason_ = client_abort_reason::none;
    }
    void abort_output(client_abort_reason reason) noexcept;
    void cancel(client_abort_reason reason) noexcept;
    void close() noexcept;
    // The caller must first join every operation borrowing the previous stream.
    void prepare_reconnect(asio::ssl::context& tls);
    void mark_tls_started() noexcept {
        tls_started_ = true;
    }
    void stop_output() noexcept;
    void bind_response(http_client_response_state* response) noexcept {
        response_ = response;
    }
    [[nodiscard]] http_client_response_state* response() const noexcept {
        return response_;
    }
    [[nodiscard]] bool arm_deadline(const operation_timeout& timeout, client_deadline_kind kind);
    [[nodiscard]] bool clear_deadline() noexcept;
    void throw_if_aborted() const;
    [[nodiscard]] http_client_error::code_type error_code(const std::error_code& error) const noexcept;
    [[nodiscard]] task<void> write(std::string_view bytes, const operation_timeout& timeout);
    [[nodiscard]] task<std::size_t> read_some(std::span<char> bytes, const operation_timeout& timeout, bool allow_eof = false);

private:
    const worker_handle& worker_;
    const http_client_config_storage& config_;
    client_wire_counters& counters_;
    asio::ip::tcp::resolver resolver_;
    asio::ssl::stream<asio::ip::tcp::socket> stream_;
    operation_deadline<client_deadline_kind> deadline_;
    std::unique_ptr<worker_timer_registration, pmr_object_deleter<worker_timer_registration>> timer_;
    client_abort_reason abort_reason_{client_abort_reason::none};
    // Borrowed only while the serialized HTTP/1 operation remains active.
    http_client_response_state* response_{};
    bool tls_started_{};
};

}  // namespace ruvia::detail
