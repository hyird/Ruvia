#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <system_error>
#include <type_traits>
#include <utility>

#include <asio/associated_allocator.hpp>
#include <asio/associated_cancellation_slot.hpp>
#include <asio/associated_executor.hpp>
#include <asio/bind_cancellation_slot.hpp>
#include <asio/buffer.hpp>
#include <asio/cancellation_signal.hpp>
#include <asio/error.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/post.hpp>
#include <asio/ssl/stream.hpp>

#include "connection_lifecycle.h"
#include "connection_scanner.h"
#include "task.h"
#include "worker_ref.h"

// The accepted server socket as a stream: prefix replay and lifecycle byte
// accounting, its TLS transport, and how a TCP connection session closes.
namespace ruvia::detail {

// The caller owns the socket and the already-read prefix until all I/O joins.
// Prefix delivery is asynchronous, like socket reads, and never reads a byte twice.
class inbound_socket_stream final {
public:
    using executor_type = asio::ip::tcp::socket::executor_type;
    using lowest_layer_type = asio::ip::tcp::socket;

    explicit inbound_socket_stream(asio::ip::tcp::socket& socket, std::span<const std::byte> prefix = {}, connection_lifecycle* lifecycle = nullptr) noexcept :
        socket_(socket),
        prefix_(prefix),
        lifecycle_(lifecycle) {}

    executor_type get_executor() noexcept {
        return socket_.get_executor();
    }
    lowest_layer_type& lowest_layer() noexcept {
        return socket_;
    }
    const lowest_layer_type& lowest_layer() const noexcept {
        return socket_;
    }
    auto native_handle() {
        return socket_.native_handle();
    }
    bool non_blocking() const {
        return socket_.non_blocking();
    }
    void shutdown(asio::ip::tcp::socket::shutdown_type what, std::error_code& error) {
        socket_.shutdown(what, error);
    }
    template <typename buffers_type>
    std::size_t send(const buffers_type& buffers, asio::socket_base::message_flags flags, std::error_code& error) {
        const auto bytes = socket_.send(buffers, flags, error);
        if (lifecycle_ != nullptr) {
            lifecycle_->note_write(bytes);
            lifecycle_->note_error(error);
        }
        return bytes;
    }

    template <typename buffers_type>
    std::size_t read_some(const buffers_type& buffers, std::error_code& error) {
        if (!prefix_.empty()) {
            error.clear();
            return consume_prefix(buffers);
        }
        const auto bytes = socket_.read_some(buffers, error);
        if (lifecycle_ != nullptr) {
            lifecycle_->note_read(bytes);
            lifecycle_->note_error(error);
        }
        return bytes;
    }

    template <typename buffers_type, typename handler_type>
    void async_read_some(const buffers_type& buffers, handler_type&& handler) {
        if (prefix_.empty()) {
            socket_.async_read_some(buffers, completion<std::decay_t<handler_type>, false>{std::forward<handler_type>(handler), socket_.get_executor(), lifecycle_});
            return;
        }
        const auto bytes = consume_prefix(buffers);
        asio::post(socket_.get_executor(), [handler = std::forward<handler_type>(handler), bytes]() mutable {
            std::move(handler)(std::error_code{}, bytes);
        });
    }

    template <typename buffers_type, typename handler_type>
    void async_write_some(const buffers_type& buffers, handler_type&& handler) {
        socket_.async_write_some(buffers, completion<std::decay_t<handler_type>, true>{std::forward<handler_type>(handler), socket_.get_executor(), lifecycle_});
    }

private:
    template <typename handler_type, bool writing>
    struct completion final {
        handler_type handler_;
        inbound_socket_stream::executor_type fallback_executor_;
        connection_lifecycle* lifecycle_;
        using executor_type = asio::associated_executor_t<handler_type, inbound_socket_stream::executor_type>;
        using allocator_type = asio::associated_allocator_t<handler_type>;
        using cancellation_slot_type = asio::associated_cancellation_slot_t<handler_type>;
        [[nodiscard]] executor_type get_executor() const noexcept {
            return asio::get_associated_executor(handler_, fallback_executor_);
        }
        [[nodiscard]] allocator_type get_allocator() const noexcept {
            return asio::get_associated_allocator(handler_);
        }
        [[nodiscard]] cancellation_slot_type get_cancellation_slot() const noexcept {
            return asio::get_associated_cancellation_slot(handler_);
        }
        void operator()(const std::error_code& error, std::size_t bytes) {
            if (lifecycle_ != nullptr) {
                if constexpr (writing) {
                    lifecycle_->note_write(bytes);
                } else {
                    lifecycle_->note_read(bytes);
                }
                // Protocol drivers also cancel reads for a successful drain.
                // Timeout, shutdown and explicit abort record their cause first.
                if (error != asio::error::operation_aborted && error != asio::error::bad_descriptor) {
                    lifecycle_->note_error(error);
                }
            }
            std::move(handler_)(error, bytes);
        }
    };
    template <typename buffers_type>
    std::size_t consume_prefix(const buffers_type& buffers) noexcept {
        const auto bytes = asio::buffer_copy(buffers, asio::buffer(prefix_.data(), prefix_.size()));
        prefix_ = prefix_.subspan(bytes);
        return bytes;
    }
    asio::ip::tcp::socket& socket_;
    std::span<const std::byte> prefix_;
    connection_lifecycle* lifecycle_;
};

// SSL 的读操作也可能发送 KeyUpdate 等控制记录。取消读允许保留已接收的
// 密文字节，但不能截断已从 SSL BIO 取出的输出，否则后续复用会丢失记录。
// 屏蔽单操作的写取消；socket 关闭和既有连接超时仍能终止底层写入。
class tls_socket_transport final {
public:
    using executor_type = asio::ip::tcp::socket::executor_type;
    using lowest_layer_type = asio::ip::tcp::socket;

    explicit tls_socket_transport(inbound_socket_stream& socket) noexcept :
        socket_(socket) {}

    [[nodiscard]] executor_type get_executor() noexcept {
        return socket_.get_executor();
    }

    [[nodiscard]] lowest_layer_type& lowest_layer() noexcept {
        return socket_.lowest_layer();
    }

    [[nodiscard]] const lowest_layer_type& lowest_layer() const noexcept {
        return socket_.lowest_layer();
    }

    template <typename buffers_type, typename handler_type>
    auto async_read_some(const buffers_type& buffers, handler_type&& handler) {
        return socket_.async_read_some(buffers, std::forward<handler_type>(handler));
    }

    template <typename buffers_type, typename handler_type>
    auto async_write_some(const buffers_type& buffers, handler_type&& handler) {
        return socket_.async_write_some(buffers, asio::bind_cancellation_slot(asio::cancellation_slot{}, std::forward<handler_type>(handler)));
    }

private:
    inbound_socket_stream& socket_;
};

using server_tls_stream = asio::ssl::stream<tls_socket_transport>;

// How a TCP connection session ended, which decides how its socket closes.
// A stopping worker closes every connection immediately, whatever its mode.
enum class http_connection_close : std::uint8_t {
    // Transport failure, a stalled transfer, or a vanished peer: close at once.
    abort,
    // No further peer input is expected: the peer ended its input (FIN or TLS
    // close_notify), or this side ended a wait for a request head (timeout or
    // drain). TLS sends close_notify first (RFC 9112 §9.8, RFC 9846 §6.1).
    notify,
    // This side ended the connection while peer input may still arrive: after
    // the final HTTP/1 response, or after HTTP/2 GOAWAY. Lingering close
    // (RFC 9112 §9.6): TLS close_notify, TCP send shutdown, then peer input is
    // read and discarded until its end, a deadline or a byte budget, so unread
    // input cannot make TCP reset the connection and destroy the last response.
    linger,
};

// Half-closes plain TCP after the final response. The session's close still
// lingers and closes the socket.
inline void shutdown_http_connection_send(asio::ip::tcp::socket& socket) noexcept {
    std::error_code ignored;
    socket.shutdown(asio::ip::tcp::socket::shutdown_send, ignored);
}

// The lingering close of a plain connection, short of the final TCP close.
// The caller closes TCP after completion and must not start this operation
// once the worker is stopping; scanner close_all, a fixed deadline and the
// worker scanner bound it. The task borrows its arguments until completion.
[[nodiscard]] task<void> linger_http_connection(asio::ip::tcp::socket& socket, connection_scanner& scanner, const worker_ref& worker_value);

// Sends TLS close_notify for a non-aborted session without waiting for the
// peer's alert, then lingers like linger_http_connection() when `mode` is
// http_connection_close::linger. Same bounds and caller duties as above.
[[nodiscard]] task<void> close_http_connection_gracefully(server_tls_stream& stream, http_connection_close mode, connection_scanner& scanner, const worker_ref& worker_value);

}  // namespace ruvia::detail
