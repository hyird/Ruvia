#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <optional>
#include <span>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>

#include <asio/append.hpp>
#include <asio/associator.hpp>
#include <asio/buffer.hpp>
#include <asio/detail/handler_cont_helpers.hpp>
#include <asio/error.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/post.hpp>
#include <asio/ssl/context.hpp>
#include <asio/ssl/stream.hpp>

#include "ruvia/core/operation_deadline.h"
#include "ruvia/core/task.h"
#include "ruvia/core/worker_ref.h"
#include "ruvia/core/worker_timer.h"

#include "client/client_happy_eyeballs.h"
#include "client/client_tls.h"
#include "server/connection_lifecycle.h"

namespace ruvia::detail {

class client_tls_socket;

// Completion of a socket write in flight: one that waits for the reactor,
// ended inside a record, or finished synchronously and must still observe a
// cancel() before the stream continues.
template <typename handler_type>
class client_tls_write_handler final {
public:
    client_tls_write_handler(handler_type&& handler, client_tls_socket& socket, bool counted)
        : handler_(std::move(handler)),
          socket_(&socket),
          counted_(counted) {}

    void operator()(const std::error_code& error, std::size_t bytes);

    // Asio's associators and continuation hook forward to the wrapped handler.
    handler_type handler_;

private:
    client_tls_socket* socket_;
    bool counted_;
};

// The TCP layer under the client's TLS stream. Every TLS record reaches the
// socket through async_write_some() here, whichever stream operation produced
// it: a write, the handshake, or a read answering a post-handshake message
// (a TLS 1.2 HelloRequest makes the read send a no_renegotiation alert). So
// this layer alone knows when each record handed to the socket is whole on
// the wire and none was lost to a failed or cancelled write: only then can
// another record follow without corrupting the stream. The socket must be in
// non-blocking mode; a write the kernel takes whole at once is complete even
// though its handler runs later.
class client_tls_socket final {
public:
    using executor_type = asio::ip::tcp::socket::executor_type;
    using lowest_layer_type = client_tls_socket;
    using native_handle_type = asio::ip::tcp::socket::native_handle_type;
    using protocol_type = asio::ip::tcp;
    using endpoint_type = asio::ip::tcp::endpoint;

    explicit client_tls_socket(const executor_type& executor)
        : socket_(executor) {}

    [[nodiscard]] executor_type get_executor() noexcept {
        return socket_.get_executor();
    }
    [[nodiscard]] lowest_layer_type& lowest_layer() noexcept {
        return *this;
    }
    [[nodiscard]] const lowest_layer_type& lowest_layer() const noexcept {
        return *this;
    }
    // The raw socket, for plaintext connections, socket options and the
    // CONNECT tunnel, which writes its own TLS records.
    [[nodiscard]] asio::ip::tcp::socket& socket() noexcept {
        return socket_;
    }
    // Replaces the unopened pre-handshake socket with the winner of the
    // Happy Eyeballs race. No TLS record operation may be active yet.
    void replace_socket(asio::ip::tcp::socket socket) noexcept {
        if (writes_ != 0) {
            std::terminate();
        }
        socket_ = std::move(socket);
        reset();
    }
    // Every record the stream handed to the socket is whole on the wire, in
    // sequence: no write is pending or partly done, and none failed.
    [[nodiscard]] bool records_whole() const noexcept {
        return writes_ == 0 && !write_failed_;
    }
    [[nodiscard]] bool is_open() const noexcept {
        return socket_.is_open();
    }
    [[nodiscard]] native_handle_type native_handle() {
        return socket_.native_handle();
    }
    void open(const protocol_type& protocol = asio::ip::tcp::v4()) {
        reset();
        socket_.open(protocol);
    }
    void open(const protocol_type& protocol, std::error_code& error) noexcept {
        reset();
        (void)socket_.open(protocol, error);
    }
    void connect(const endpoint_type& endpoint) {
        reset();
        socket_.connect(endpoint);
    }
    void connect(const endpoint_type& endpoint, std::error_code& error) noexcept {
        reset();
        (void)socket_.connect(endpoint, error);
    }
    void cancel() {
        std::error_code error;
        cancel(error);
        if (error) {
            throw std::system_error(error);
        }
    }
    void cancel(std::error_code& error) noexcept {
        cancelled_ = true;
        if (writes_ != 0) {
            write_failed_ = true;
        }
        (void)socket_.cancel(error);
    }
    void close() {
        std::error_code error;
        close(error);
        if (error) {
            throw std::system_error(error);
        }
    }
    void close(std::error_code& error) noexcept {
        cancelled_ = true;
        if (writes_ != 0) {
            write_failed_ = true;
        }
        std::error_code ignored;
        (void)socket_.cancel(ignored);
        (void)socket_.close(error);
    }
    void reset() noexcept {
        cancelled_ = false;
        write_failed_ = false;
    }

    template <typename buffers_type>
    std::size_t read_some(const buffers_type& buffers, std::error_code& error) {
        return socket_.read_some(buffers, error);
    }
    template <typename buffers_type>
    std::size_t write_some(const buffers_type& buffers, std::error_code& error) {
        return socket_.write_some(buffers, error);
    }
    template <typename buffers_type, typename handler_type>
    void async_read_some(const buffers_type& buffers, handler_type&& handler) {
        if (cancelled_) {
            asio::post(socket_.get_executor(),
                asio::append(std::forward<handler_type>(handler), asio::error::operation_aborted, std::size_t{0}));
            return;
        }
        socket_.async_read_some(buffers, std::forward<handler_type>(handler));
    }
    template <typename buffers_type, typename handler_type>
    void async_write_some(const buffers_type& buffers, handler_type&& handler) {
        using completion_type = client_tls_write_handler<std::decay_t<handler_type>>;
        if (cancelled_) {
            write_failed_ = true;
            asio::post(socket_.get_executor(),
                asio::append(std::forward<handler_type>(handler), asio::error::operation_aborted, std::size_t{0}));
            return;
        }
        // Like Asio's own speculative write, but its outcome stays visible:
        // only a write that waits for the reactor or ends inside a record is
        // in flight. Queued writes keep their order, so a direct write runs
        // only while none waits.
        if (writes_ == 0 && socket_.non_blocking()) {
            std::error_code error;
            const auto written = socket_.write_some(buffers, error);
            if (error != asio::error::would_block && error != asio::error::try_again &&
                error != asio::error::interrupted) {
                if (error) {
                    write_failed_ = true;
                    asio::post(socket_.get_executor(), asio::append(std::forward<handler_type>(handler), error, written));
                    return;
                }
                const bool partial = written != asio::buffer_size(buffers);
                if (partial) {
                    ++writes_;
                }
                try {
                    asio::post(socket_.get_executor(),
                        asio::append(completion_type(std::forward<handler_type>(handler), *this, partial), error, written));
                } catch (...) {
                    if (partial) {
                        --writes_;
                    }
                    write_failed_ = true;
                    throw;
                }
                return;
            }
        }
        ++writes_;
        try {
            socket_.async_write_some(buffers, completion_type(std::forward<handler_type>(handler), *this, true));
        } catch (...) {
            --writes_;
            write_failed_ = true;
            throw;
        }
    }

private:
    template <typename>
    friend class client_tls_write_handler;

    asio::ip::tcp::socket socket_;
    std::size_t writes_{0};
    // A failed or cancelled write may leave a record partly sent, or records
    // unsent whose sequence numbers a later record would skip.
    bool write_failed_{false};
    bool cancelled_{false};
};

template <typename handler_type>
void client_tls_write_handler<handler_type>::operator()(const std::error_code& error, std::size_t bytes) {
    if (counted_) {
        --socket_->writes_;
    }
    if (socket_->cancelled_) {
        socket_->write_failed_ = true;
        std::move(handler_)(asio::error::operation_aborted, 0);
        return;
    }
    if (error) {
        socket_->write_failed_ = true;
    }
    std::move(handler_)(error, bytes);
}

template <typename handler_type>
[[nodiscard]] bool asio_handler_is_continuation(client_tls_write_handler<handler_type>* handler) {
    return asio_handler_cont_helpers::is_continuation(handler->handler_);
}

}  // namespace ruvia::detail

namespace asio {

template <template <typename, typename> class associator_type, typename handler_type, typename candidate_type>
struct associator<associator_type, ruvia::detail::client_tls_write_handler<handler_type>, candidate_type>
    : associator_type<handler_type, candidate_type> {
    static typename associator_type<handler_type, candidate_type>::type get(
        const ruvia::detail::client_tls_write_handler<handler_type>& handler) noexcept {
        return associator_type<handler_type, candidate_type>::get(handler.handler_);
    }
    static auto get(const ruvia::detail::client_tls_write_handler<handler_type>& handler,
        const candidate_type& candidate) noexcept
        -> decltype(associator_type<handler_type, candidate_type>::get(handler.handler_, candidate)) {
        return associator_type<handler_type, candidate_type>::get(handler.handler_, candidate);
    }
};

}  // namespace asio

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

enum class client_idle_transport_status : std::uint8_t {
    reusable,
    closed,
    pending_data,
};

// Borrowed origin of one transport; the owner's startup config outlives it.
struct client_tcp_endpoint final {
    // Validated unbracketed host used for DNS.
    std::string_view host_;
    std::uint16_t port_;
    // nullptr selects plaintext TCP.
    const client_tls_config_storage* tls_;
    client_tcp_config tcp_;
    // ALPN wire offer borrowed until transport retirement; empty for TCP.
    std::span<const unsigned char> alpn_{};
    // TLS SNI and verified peer name, used verbatim; empty selects the TLS
    // configuration's server name, else host_.
    std::string_view identity_host_{};
    void* resolve_context_{};
    // Resolves host_ and port_ in place of the transport's own resolver.
    task<client_resolve_result> (*resolve_)(void*, client_host_resolver&, const operation_deadline&, std::string_view, std::uint16_t){};
};

// True for an OpenSSL failure or a TLS stream truncated without close_notify.
[[nodiscard]] bool is_tls_error(const std::error_code& error) noexcept;

// The one outbound TCP/TLS byte transport shared by the HTTP/1, WebSocket and
// Redis clients. Protocol-neutral: callers map results onto their error
// types. One worker owns the socket; the read lane (resolve, connect, TLS
// handshake, reads) and the write lane have independent deadlines so a
// full-duplex exchange can bound its upload separately. A connected socket
// stays in non-blocking mode: every operation on it is asynchronous or a
// bounded probe. Address-stable. Construction throws std::invalid_argument
// for a DNS host on a worker without a blocking pool.
class client_tcp_transport final {
public:
    client_tcp_transport(asio::io_context& io, asio::ssl::context& tls, const worker_ref& worker_value,
        client_tcp_endpoint endpoint, client_wire_counters& counters);
    client_tcp_transport(asio::io_context&, asio::ssl::context&, worker_ref&&, client_tcp_endpoint,
        client_wire_counters&) = delete;
    ~client_tcp_transport();
    client_tcp_transport(const client_tcp_transport&) = delete;
    client_tcp_transport& operator=(const client_tcp_transport&) = delete;

    [[nodiscard]] bool secure() const noexcept {
        return endpoint_.tls_ != nullptr;
    }
    [[nodiscard]] const client_tcp_endpoint& endpoint() const noexcept {
        return endpoint_;
    }
    // Resolves, connects, applies socket policies and, for TLS, prepares SNI,
    // peer identity and the ALPN offer before the handshake. h2-only offers
    // require TLS 1.2; a peer selecting h2 below TLS 1.2 is rejected before
    // application I/O. Every operation borrowing a previous connection must
    // have finished.
    [[nodiscard]] task<client_transport_result> connect(const operation_deadline& timeout);
    // Replaces the endpoint's ALPN offer for every later connect().
    void offer_alpn(client_alpn alpn) noexcept {
        endpoint_.alpn_ = client_alpn_bytes(alpn);
    }
    [[nodiscard]] const asio::ip::tcp::endpoint& peer_endpoint() const noexcept {
        return peer_endpoint_;
    }
    [[nodiscard]] std::chrono::steady_clock::duration connect_duration() const noexcept {
        return connect_duration_;
    }
    [[nodiscard]] std::chrono::steady_clock::duration tls_duration() const noexcept {
        return tls_duration_;
    }
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
    [[nodiscard]] task<client_transport_result> read_some(std::span<char> bytes, const operation_deadline& timeout);
    // Writes every byte under the write lane's own deadline. The buffer
    // sequence form sends framed parts (chunk prefix, payload, suffix) as one
    // transport write, which is one TLS record sequence and one deadline cycle.
    [[nodiscard]] task<client_transport_result> write(std::string_view bytes, const operation_deadline& timeout);
    [[nodiscard]] task<client_transport_result> write(
        std::span<const asio::const_buffer> buffers, const operation_deadline& timeout);

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
    // runs the TLS engine. An HTTP/1 or Redis peer sends nothing between
    // exchanges, so readable or TLS-buffered bytes are distinguished from an
    // end of stream before either condition retires the connection. Pending TLS
    // data can be close_notify, a post-handshake record or an unsolicited response.
    [[nodiscard]] client_idle_transport_status idle_status() noexcept;
    // Completes once the socket of a connected, idle transport is readable,
    // at its end or failed, without reading and without running the TLS
    // engine; idle_status() then classifies it. Only while no other operation
    // is pending: cancel_wait() ends it early.
    [[nodiscard]] task<void> wait_readable();
    void cancel_wait() noexcept;

    // Raw stream for the CONNECT tunnel adapter, which owns TLS shutdown: the
    // transport no longer sends close_notify for this connection.
    [[nodiscard]] asio::ssl::stream<client_tls_socket>& stream() noexcept {
        tls_close_ready_ = false;
        return stream_;
    }

private:
    struct lane_deadline final {
        deadline_lifecycle<> deadline_;
        worker_timer_registration timer_;
    };

    template <typename buffers_type>
    [[nodiscard]] task<client_transport_result> write_buffers(buffers_type buffers, const operation_deadline& timeout);
    void send_close_notify() noexcept;
    [[nodiscard]] bool arm(lane_deadline& lane, const operation_deadline& timeout);
    [[nodiscard]] bool clear(lane_deadline& lane) noexcept;
    [[nodiscard]] client_transport_result finish(lane_deadline& lane, const operation_deadline& timeout,
        const std::error_code& error, std::size_t bytes, client_transport_status failure) noexcept;
    [[nodiscard]] client_transport_result aborted() const noexcept;

    const worker_ref& worker_;
    asio::ssl::context& tls_;
    client_tcp_endpoint endpoint_;
    std::optional<asio::ip::tcp::endpoint> literal_endpoint_{};
    client_wire_counters& counters_;
    asio::ip::tcp::endpoint peer_endpoint_;
    std::chrono::steady_clock::duration connect_duration_{};
    std::chrono::steady_clock::duration tls_duration_{};
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
