#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <system_error>

#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/ssl/context.hpp>
#include <asio/ssl/stream.hpp>

#include "ruvia/core/operation_deadline.h"
#include "ruvia/core/operation_timeout.h"
#include "ruvia/core/task.h"
#include "ruvia/core/worker_handle.h"
#include "ruvia/core/worker_timer.h"
#include "ruvia/web/client_tcp_config.h"

#include "client/client_tls_config_storage.h"
#include "client/client_transport.h"

namespace ruvia::detail {

// Why an owner stopped a transport. The latest non-none reason wins until the
// owner starts a new operation with reset_abort().
enum class client_abort_reason : std::uint8_t { none,
    timeout,
    cancelled,
    closing };

enum class client_transport_status : std::uint8_t {
    ok,
    // The peer ended the byte stream (TCP FIN, close_notify or truncation).
    eof,
    // abort_ names the reason: an owner abort or an expired deadline.
    aborted,
    resolve_failed,
    connect_failed,
    tls_failed,
    io_failed,
};

struct client_transport_result final {
    client_transport_status status_{client_transport_status::ok};
    client_abort_reason abort_{client_abort_reason::none};
    std::size_t bytes_{0};
    std::error_code error_{};

    [[nodiscard]] bool ok() const noexcept {
        return status_ == client_transport_status::ok;
    }
};

struct client_wire_counters final {
    std::size_t sent_{};
    std::size_t received_{};
};

// Borrowed origin of one transport; the owner's startup config outlives it.
struct client_tcp_endpoint final {
    // Validated unbracketed host used for DNS.
    std::string_view host_;
    std::uint16_t port_;
    // nullptr selects plaintext TCP.
    const client_tls_config_storage* tls_;
    client_tcp_config tcp_;
};

// The one outbound TCP/TLS byte transport shared by the HTTP/1, HTTP/2 and
// WebSocket clients. Protocol-neutral: callers map results onto their error
// types. One worker owns the socket; the read lane (resolve, connect, TLS
// handshake, reads) and the write lane have independent deadlines so a
// full-duplex exchange can bound its upload separately. Address-stable.
class client_tcp_transport final {
public:
    client_tcp_transport(asio::io_context& io, asio::ssl::context& tls, const worker_handle& worker_value,
        client_tcp_endpoint endpoint, client_wire_counters& counters);
    client_tcp_transport(asio::io_context&, asio::ssl::context&, worker_handle&&, client_tcp_endpoint,
        client_wire_counters&) = delete;
    ~client_tcp_transport();
    client_tcp_transport(const client_tcp_transport&) = delete;
    client_tcp_transport& operator=(const client_tcp_transport&) = delete;

    [[nodiscard]] bool secure() const noexcept {
        return endpoint_.tls_ != nullptr;
    }
    // Resolves, connects, applies socket policies and, for TLS, prepares SNI,
    // peer identity and `alpn` before the handshake. Every operation borrowing
    // a previous connection must have finished.
    [[nodiscard]] task<client_transport_result> connect(client_alpn_mode alpn, const operation_timeout& timeout);
    // Selected ALPN protocol of the current TLS connection; empty for TCP.
    [[nodiscard]] std::string_view alpn() noexcept;
    [[nodiscard]] task<client_transport_result> read_some(std::span<char> bytes, const operation_timeout& timeout);
    // Writes every byte under the write lane's own deadline.
    [[nodiscard]] task<client_transport_result> write(std::string_view bytes, const operation_timeout& timeout);

    [[nodiscard]] client_abort_reason abort_reason() const noexcept {
        return abort_reason_;
    }
    void reset_abort() noexcept {
        abort_reason_ = client_abort_reason::none;
    }
    // Records the reason (replacing an earlier one) and cancels pending
    // resolve and socket operations; the socket stays open until close().
    void abort(client_abort_reason reason) noexcept;
    void close() noexcept;

    // Raw stream for the CONNECT tunnel adapter, which owns TLS shutdown.
    [[nodiscard]] asio::ssl::stream<asio::ip::tcp::socket>& stream() noexcept {
        return stream_;
    }

private:
    enum class lane_type : std::uint8_t { read,
        write };

    struct lane_deadline final {
        operation_deadline<lane_type> deadline_;
        worker_timer_registration timer_;
    };

    [[nodiscard]] bool arm(lane_deadline& lane, lane_type kind, const operation_timeout& timeout);
    [[nodiscard]] bool clear(lane_deadline& lane) noexcept;
    [[nodiscard]] client_transport_result finish(lane_deadline& lane, const operation_timeout& timeout,
        const std::error_code& error, std::size_t bytes, client_transport_status failure) noexcept;
    [[nodiscard]] client_transport_result aborted() const noexcept;

    const worker_handle& worker_;
    asio::ssl::context& tls_;
    client_tcp_endpoint endpoint_;
    client_wire_counters& counters_;
    asio::ip::tcp::resolver resolver_;
    asio::ssl::stream<asio::ip::tcp::socket> stream_;
    lane_deadline read_;
    lane_deadline write_;
    client_abort_reason abort_reason_{client_abort_reason::none};
    bool tls_started_{false};
};

}  // namespace ruvia::detail
