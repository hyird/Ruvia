#include "client/client_tcp_transport.h"

#include <chrono>
#include <utility>

#include <asio/ssl/error.hpp>
#include <asio/write.hpp>
#include <openssl/bio.h>
#include <openssl/ssl.h>

#include "ruvia/core/async.h"

#include "client/client_happy_eyeballs.h"
#include "tls/tls_stream_end.h"
#include "util/tcp_socket_policies.h"

namespace ruvia::detail {

bool is_tls_error(const std::error_code& error) noexcept {
    return error.category() == asio::error::get_ssl_category() || error == asio::ssl::error::stream_truncated;
}

client_tcp_transport::client_tcp_transport(asio::io_context& io, asio::ssl::context& tls,
    const worker_handle& worker_value, client_tcp_endpoint endpoint, client_wire_counters& counters)
    : worker_(worker_value),
      tls_(tls),
      endpoint_(endpoint),
      counters_(counters),
      resolver_(worker_value),
      stream_(io.get_executor(), tls) {}

client_tcp_transport::~client_tcp_transport() = default;

void client_tcp_transport::abort(client_abort_reason reason) noexcept {
    // The latest owner decision wins: a pool shutdown reports closing even if
    // a request cancellation reached the transport first.
    if (reason != client_abort_reason::none) {
        abort_reason_ = reason;
    }
    if (writing_ || reason == client_abort_reason::timeout || reason == client_abort_reason::cancelled) {
        tls_close_ready_ = false;
    }
    resolver_.cancel();
    std::error_code ignored;
    if (connecting_ != nullptr) {
        connecting_->cancel();
    } else {
        (void)stream_.lowest_layer().cancel(ignored);
    }
}

void client_tcp_transport::close() noexcept {
    read_.timer_.cancel();
    read_.deadline_.reset();
    write_.timer_.cancel();
    write_.deadline_.reset();
    resolver_.cancel();
    if (connecting_ != nullptr) {
        connecting_->cancel();
    }
    send_close_notify();
    std::error_code ignored;
    (void)stream_.lowest_layer().cancel(ignored);
    (void)stream_.lowest_layer().close(ignored);
}

void client_tcp_transport::send_close_notify() noexcept {
    if (!tls_close_ready_ || writing_) {
        return;
    }
    tls_close_ready_ = false;
    auto& layer = stream_.next_layer();
    auto& socket = layer.socket();
    SSL* const native = stream_.native_handle();
    // One worker runs every engine call, so the engine orders the alert after
    // the records of the pending read and write. On the socket it must follow
    // them whole too: while a record write waits for the socket or ended
    // inside a record (from a write, or from a read answering a post-handshake
    // message) the alert would land inside that record, and after a failed or
    // cancelled write it would follow a cut or lost one, so the close goes
    // without the alert. A post-handshake message still being read keeps the
    // engine in init.
    if (!layer.records_whole() || !socket.is_open() || SSL_is_init_finished(native) != 1 ||
        (SSL_get_shutdown(native) & SSL_SENT_SHUTDOWN) != 0) {
        return;
    }
    // Send our alert without waiting for the peer's: recording its alert as
    // received makes SSL_shutdown complete after the one write on the
    // non-blocking socket, which first flushes engine output an operation
    // left unsent. The engine is never read again; a full socket buffer fails
    // the write and the close proceeds without the alert.
    SSL_set_shutdown(native, SSL_get_shutdown(native) | SSL_RECEIVED_SHUTDOWN);
    std::error_code error;
    (void)stream_.shutdown(error);
}

// An idle pooled connection is reusable only while the peer has sent nothing:
// unsolicited bytes are a stray response or the start of a close. The probe is
// a constant-time inspection that never runs the TLS engine: a decrypting read
// on the worker keeps processing records for as long as the peer keeps sending
// post-handshake control records (TLS 1.3 NewSessionTicket, KeyUpdate), and
// nonblocking mode does not bound that loop. Without decrypting, those records
// cannot be told from a close_notify or a stray response, so any record
// OpenSSL already buffers (decrypted or not) or any byte waiting on the socket
// retires the connection. Servers send their tickets right after the
// handshake, ahead of the first response, so the response read consumes them
// and an idle connection normally carries none; a later control record only
// costs one reconnect. Raw bytes Asio holds in its own input buffer stay
// invisible here, like a close racing the probe: the response read reports them.
client_idle_transport_status client_tcp_transport::idle_status() noexcept {
    auto& socket = stream_.next_layer().socket();
    if (!socket.is_open()) {
        return client_idle_transport_status::closed;
    }
    if (secure()) {
        SSL* const native = stream_.native_handle();
        if (SSL_has_pending(native) != 0 || BIO_ctrl_pending(SSL_get_rbio(native)) != 0) {
            return client_idle_transport_status::pending_data;
        }
    }
    std::error_code error;
    char byte{};
    const auto bytes = socket.receive(
        asio::buffer(&byte, 1), asio::socket_base::message_peek, error);
    if (error == asio::error::would_block) {
        return client_idle_transport_status::reusable;
    }
    return !error && bytes != 0 ? client_idle_transport_status::pending_data
                                : client_idle_transport_status::closed;
}

bool client_tcp_transport::arm(lane_deadline& lane, const operation_timeout& timeout) {
    lane.timer_.cancel();
    const auto remaining = timeout.remaining();
    if (!remaining) {
        lane.deadline_.reset();
        return true;
    }
    if (remaining->count() == 0) {
        lane.deadline_.reset();
        return false;
    }
    const auto deadline_value = *timeout.deadline();
    lane.deadline_.arm(deadline_value);
    (void)worker_.schedule_timer(lane.timer_, deadline_value, [this, &lane](worker_timer_outcome outcome) noexcept {
        if (outcome == worker_timer_outcome::expired && lane.deadline_.expire(std::chrono::steady_clock::now())) {
            abort(client_abort_reason::timeout);
        }
    });
    return true;
}

bool client_tcp_transport::clear(lane_deadline& lane) noexcept {
    lane.timer_.cancel();
    return lane.deadline_.clear();
}

client_transport_result client_tcp_transport::aborted() const noexcept {
    return {.status_ = client_transport_status::aborted,
        .abort_ = abort_reason_ == client_abort_reason::none ? client_abort_reason::timeout : abort_reason_};
}

client_transport_result client_tcp_transport::finish(lane_deadline& lane, const operation_timeout& timeout,
    const std::error_code& error, std::size_t bytes, client_transport_status failure) noexcept {
    if ((clear(lane) || timeout.expired()) && abort_reason_ == client_abort_reason::none) {
        abort_reason_ = client_abort_reason::timeout;
    }
    if (abort_reason_ != client_abort_reason::none) {
        tls_close_ready_ = false;
        return aborted();
    }
    if (error) {
        if (failure == client_transport_status::io_failed && is_stream_read_end(error)) {
            return {.status_ = client_transport_status::eof, .error_ = error};
        }
        const auto status = secure() && tls_started_ && is_tls_error(error) ? client_transport_status::tls_failed : failure;
        return {.status_ = status, .bytes_ = bytes, .error_ = error};
    }
    return {.bytes_ = bytes};
}

task<client_transport_result> client_tcp_transport::connect(const operation_timeout& timeout) {
    tls_close_ready_ = false;
    writing_ = false;
    if (tls_started_) {
        // Closing a socket leaves Asio's encrypted input cursor and BIO pair
        // alive. Swap through a fresh stream so the retired SSL engine is
        // destroyed without overwriting its native owner, and the socket
        // layer's record accounting starts over.
        asio::ssl::stream<client_tls_socket> fresh(stream_.get_executor(), tls_);
        std::swap(stream_, fresh);
        tls_started_ = false;
    } else {
        stream_.next_layer().reset();
    }
    if (!arm(read_, timeout)) {
        co_return aborted();
    }
    auto resolved = co_await resolver_.resolve(endpoint_.host_, endpoint_.port_, timeout);
    if (abort_reason_ != client_abort_reason::none || read_.deadline_.expired() || timeout.expired()) {
        co_return finish(read_, timeout, {}, 0, client_transport_status::resolve_failed);
    }
    if (resolved.error_) {
        co_return finish(read_, timeout, resolved.error_, 0, client_transport_status::resolve_failed);
    }
    if (resolved.literal_) {
        // One numeric endpoint needs neither a resolver result allocation nor
        // the multi-address race. The same transport deadline and cancellation
        // own this socket as in the ordinary I/O paths.
        stream_.next_layer().replace_socket(asio::ip::tcp::socket(stream_.get_executor()));
        const auto connected = co_await async_asio([this, endpoint = *resolved.literal_](auto handler) mutable {
            stream_.next_layer().socket().async_connect(endpoint, std::move(handler));
        });
        if (connected.error_code() || abort_reason_ != client_abort_reason::none ||
            read_.deadline_.expired() || timeout.expired()) {
            co_return finish(read_, timeout, connected.error_code(), 0, client_transport_status::connect_failed);
        }
    } else {
        client_happy_eyeballs connector(worker_, stream_.get_executor(), resolved.addresses_);
        connecting_ = &connector;
        struct clear_connector final {
            client_happy_eyeballs*& value_;
            ~clear_connector() {
                value_ = nullptr;
            }
        } connector_guard{connecting_};
        auto connected = co_await connector.connect();
        connecting_ = nullptr;
        if (connected.error_ || abort_reason_ != client_abort_reason::none || read_.deadline_.expired() ||
            timeout.expired()) {
            co_return finish(read_, timeout, connected.error_, 0, client_transport_status::connect_failed);
        }
        if (!connected.socket_) {
            std::terminate();
        }
        stream_.next_layer().replace_socket(std::move(*connected.socket_));
    }
    auto mode_error = apply_tcp_socket_policies(
        stream_.next_layer().socket(), endpoint_.tcp_.no_delay_, endpoint_.tcp_.keep_alive_);
    // Every socket operation is asynchronous or a bounded probe, and the TLS
    // socket layer writes records directly.
    if (!mode_error) {
        stream_.next_layer().socket().non_blocking(true, mode_error);
    }
    if (mode_error || !secure()) {
        co_return finish(read_, timeout, mode_error, 0, client_transport_status::connect_failed);
    }
    tls_started_ = true;
    if (const auto prepared = prepare_client_tls_stream(*stream_.native_handle(),
            endpoint_.tls_->tls_host(endpoint_.host_), endpoint_.tls_->mode_, endpoint_.alpn_);
        prepared != client_tls_setup_error::none) {
        (void)clear(read_);
        co_return client_transport_result{.status_ = client_transport_status::tls_failed,
            .error_ = make_client_tls_setup_error_code(prepared)};
    }
    const auto handshake = co_await async_asio([this](auto handler) mutable {
        stream_.async_handshake(asio::ssl::stream_base::client, std::move(handler));
    });
    auto result_value = finish(read_, timeout, handshake.error_code(), 0, client_transport_status::tls_failed);
    tls_close_ready_ = result_value.ok();
    co_return result_value;
}

std::string_view client_tcp_transport::alpn() noexcept {
    return tls_started_ ? selected_client_alpn(stream_.native_handle()) : std::string_view{};
}

task<client_transport_result> client_tcp_transport::read_some(std::span<char> bytes_value, const operation_timeout& timeout) {
    if (abort_reason_ != client_abort_reason::none || !arm(read_, timeout)) {
        co_return aborted();
    }
    const auto buffer = asio::buffer(bytes_value);
    const auto completion = secure()
                                ? co_await async_asio<std::size_t>([this, buffer](auto handler) mutable {
                                      stream_.async_read_some(buffer, std::move(handler));
                                  })
                                : co_await async_asio<std::size_t>([this, buffer](auto handler) mutable {
                                      stream_.next_layer().socket().async_read_some(buffer, std::move(handler));
                                  });
    // After a TLS failure the engine owes the peer no further records.
    if (is_tls_error(completion.error_code())) {
        tls_close_ready_ = false;
    }
    const auto transferred = completion.result();
    counters_.received_ += transferred;
    auto result_value = finish(read_, timeout, completion.error_code(),
        completion.error_code() ? 0 : transferred, client_transport_status::io_failed);
    if (result_value.ok() && result_value.bytes_ == 0) {
        result_value.status_ = client_transport_status::eof;
    }
    co_return result_value;
}

template <typename buffers_type>
task<client_transport_result> client_tcp_transport::write_buffers(buffers_type buffers, const operation_timeout& timeout) {
    if (abort_reason_ != client_abort_reason::none) {
        co_return aborted();
    }
    if (asio::buffer_size(buffers) == 0) {
        co_return client_transport_result{};
    }
    if (!arm(write_, timeout)) {
        tls_close_ready_ = false;
        co_return aborted();
    }
    writing_ = true;
    const auto completion = secure()
                                ? co_await async_asio<std::size_t>([this, buffers](auto handler) mutable {
                                      asio::async_write(stream_, buffers, std::move(handler));
                                  })
                                : co_await async_asio<std::size_t>([this, buffers](auto handler) mutable {
                                      asio::async_write(stream_.next_layer().socket(), buffers, std::move(handler));
                                  });
    writing_ = false;
    if (completion.error_code()) {
        tls_close_ready_ = false;
    }
    const auto transferred = completion.result();
    counters_.sent_ += transferred;
    auto result_value = finish(write_, timeout, completion.error_code(),
        completion.error_code() ? 0 : transferred, client_transport_status::io_failed);
    if (result_value.status_ == client_transport_status::eof) {
        result_value.status_ = client_transport_status::io_failed;
    }
    co_return result_value;
}

task<client_transport_result> client_tcp_transport::write(std::string_view bytes_value, const operation_timeout& timeout) {
    return write_buffers(asio::buffer(bytes_value), timeout);
}

task<client_transport_result> client_tcp_transport::write(
    std::span<const asio::const_buffer> buffers, const operation_timeout& timeout) {
    return write_buffers(buffers, timeout);
}

}  // namespace ruvia::detail
