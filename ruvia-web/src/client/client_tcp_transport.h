#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <system_error>

#include <asio/buffer.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/ssl/context.hpp>
#include <asio/ssl/stream.hpp>

#include "ruvia/core/operation_timeout.h"
#include "ruvia/core/task.h"
#include "ruvia/core/worker_handle.h"
#include "ruvia/core/worker_timer.h"

#include "client/client_host_resolver.h"
#include "client/client_tls.h"
#include "client/client_tls_config_storage.h"
#include "client/client_tls_socket.h"

namespace ruvia::detail {

class client_happy_eyeballs;

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

// Counts plaintext bytes completed by socket/TLS I/O, including the completed
// prefix of an operation that also reports an error.
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
    // ALPN offered in the TLS handshake; ignored for plaintext TCP.
    client_alpn alpn_;
};

// True for an OpenSSL failure or a TLS stream truncated without close_notify.
[[nodiscard]] bool is_tls_error(const std::error_code& error) noexcept;

// The one outbound TCP/TLS byte transport shared by the HTTP/1, WebSocket and
// Redis clients. Protocol-neutral: callers map results onto their error
// types. One worker owns the socket; the read lane (resolve, connect, TLS
// handshake, reads) and the write lane have independent deadlines so a
// full-duplex exchange can bound its upload separately. A connected socket
// stays in non-blocking mode: every operation on it is asynchronous or a
// bounded probe. Address-stable.
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
    // peer identity and the endpoint's ALPN offer before the handshake. Every
    // operation borrowing a previous connection must have finished.
    [[nodiscard]] task<client_transport_result> connect(const operation_timeout& timeout);
    // Selected ALPN protocol of the current TLS connection; empty for TCP.
    [[nodiscard]] std::string_view alpn() noexcept;
    // HTTP/1 clients offer only http/1.1: a TLS peer either skips ALPN or
    // must select it.
    [[nodiscard]] bool alpn_allows_http11() noexcept {
        if (!secure()) {
            return true;
        }
        const auto selected = alpn();
        return selected.empty() || selected == "http/1.1";
    }
    [[nodiscard]] task<client_transport_result> read_some(std::span<char> bytes, const operation_timeout& timeout);
    // Writes every byte under the write lane's own deadline. The buffer
    // sequence form sends framed parts (chunk prefix, payload, suffix) as one
    // transport write, which is one TLS record sequence and one deadline cycle.
    [[nodiscard]] task<client_transport_result> write(std::string_view bytes, const operation_timeout& timeout);
    [[nodiscard]] task<client_transport_result> write(
        std::span<const asio::const_buffer> buffers, const operation_timeout& timeout);

    [[nodiscard]] client_abort_reason abort_reason() const noexcept {
        return abort_reason_;
    }
    void reset_abort() noexcept {
        abort_reason_ = client_abort_reason::none;
    }
    // Records the reason (replacing an earlier one) and cancels pending
    // resolve and socket operations; the established socket stays open until
    // close(). During an address race, every attempt is closed and joined.
    void abort(client_abort_reason reason) noexcept;
    // Closes the connection at once. An established TLS connection first
    // sends close_notify (RFC 9112 §9.8, RFC 9846 §6.1) with one non-blocking
    // write when the alert can follow every record already sent whole: no
    // record write, from a write or from a read answering a post-handshake
    // message, still waits for the socket or ended inside a record, and none
    // failed or was cancelled. A peer that is not reading does not delay the
    // close, it only misses the alert. A pending read does not hold the alert
    // back; the close cancels it.
    void close() noexcept;
    // Bounded non-blocking check of a connected, idle transport that never
    // runs the TLS engine. An HTTP/1 peer sends nothing between exchanges, so
    // readable or TLS-buffered bytes or end of stream retire the connection
    // (FIN, TLS close_notify, post-handshake records or an unsolicited response).
    [[nodiscard]] bool idle_open() noexcept;

    // Raw stream for the CONNECT tunnel adapter, which owns TLS shutdown: the
    // transport no longer sends close_notify for this connection.
    [[nodiscard]] asio::ssl::stream<client_tls_socket>& stream() noexcept {
        tls_close_ready_ = false;
        return stream_;
    }

private:
    struct lane_deadline final {
        operation_deadline<> deadline_;
        worker_timer_registration timer_;
    };

    template <typename buffers_type>
    [[nodiscard]] task<client_transport_result> write_buffers(buffers_type buffers, const operation_timeout& timeout);
    void send_close_notify() noexcept;
    [[nodiscard]] bool arm(lane_deadline& lane, const operation_timeout& timeout);
    [[nodiscard]] bool clear(lane_deadline& lane) noexcept;
    [[nodiscard]] client_transport_result finish(lane_deadline& lane, const operation_timeout& timeout,
        const std::error_code& error, std::size_t bytes, client_transport_status failure) noexcept;
    [[nodiscard]] client_transport_result aborted() const noexcept;

    const worker_handle& worker_;
    asio::ssl::context& tls_;
    client_tcp_endpoint endpoint_;
    client_wire_counters& counters_;
    client_host_resolver resolver_;
    asio::ssl::stream<client_tls_socket> stream_;
    lane_deadline read_;
    lane_deadline write_;
    client_abort_reason abort_reason_{client_abort_reason::none};
    bool tls_started_{false};
    // Non-null only while the address race is active; abort() cancels and the
    // connect coroutine joins every attempted socket before clearing it.
    client_happy_eyeballs* connecting_{nullptr};
    // The TLS connection is established and has not failed, so a close may
    // append close_notify once its socket layer holds whole records only.
    bool tls_close_ready_{false};
    // A composed write_buffers call is in flight: it may span multiple TLS
    // records across posted speculative completions.
    bool writing_{false};
};

}  // namespace ruvia::detail
