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

#include "ruvia/core/OperationTimeout.h"
#include "ruvia/core/Task.h"
#include "ruvia/core/WorkerHandle.h"
#include "ruvia/core/WorkerTimer.h"
#include "ruvia/core/memory/PmrObject.h"
#include "ruvia/core/operation_deadline.h"
#include "ruvia/web/HttpClientTypes.h"

namespace ruvia::detail {

struct HttpClientConfigStorage;
class HttpClientResponseState;

enum class client_abort_reason : std::uint8_t { none,
    timeout,
    cancelled,
    closing };
enum class client_deadline_kind : std::uint8_t { resolve,
    socket,
    response_buffer };

struct client_wire_counters final {
    std::size_t sent{};
    std::size_t received{};
};

// One worker owns the socket, resolver, deadline and borrowed response waiter.
// Origin configuration and counters outlive every transport in the pool.
class client_tcp_transport final {
public:
    client_tcp_transport(asio::io_context& io, asio::ssl::context& tls,
        const WorkerHandle& worker, const HttpClientConfigStorage& config,
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
    void bind_response(HttpClientResponseState* response) noexcept {
        response_ = response;
    }
    [[nodiscard]] HttpClientResponseState* response() const noexcept {
        return response_;
    }
    [[nodiscard]] bool arm_deadline(const OperationTimeout& timeout, client_deadline_kind kind);
    [[nodiscard]] bool clear_deadline() noexcept;
    void throw_if_aborted() const;
    [[nodiscard]] HttpClientError::Code error_code(const std::error_code& error) const noexcept;
    [[nodiscard]] Task<void> write(std::string_view bytes, const OperationTimeout& timeout);
    [[nodiscard]] Task<std::size_t> read_some(std::span<char> bytes, const OperationTimeout& timeout, bool allow_eof = false);

private:
    const WorkerHandle& worker_;
    const HttpClientConfigStorage& config_;
    client_wire_counters& counters_;
    asio::ip::tcp::resolver resolver_;
    asio::ssl::stream<asio::ip::tcp::socket> stream_;
    operation_deadline<client_deadline_kind> deadline_;
    std::unique_ptr<WorkerTimerRegistration, PmrObjectDeleter<WorkerTimerRegistration>> timer_;
    client_abort_reason abort_reason_{client_abort_reason::none};
    // Borrowed only while the serialized HTTP/1 operation remains active.
    HttpClientResponseState* response_{};
    bool tls_started_{};
};

}  // namespace ruvia::detail
