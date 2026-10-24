#pragma once

#include <cstddef>
#include <system_error>
#include <type_traits>
#include <utility>

#include <asio/append.hpp>
#include <asio/associator.hpp>
#include <asio/buffer.hpp>
#include <asio/detail/handler_cont_helpers.hpp>
#include <asio/error.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/post.hpp>

namespace ruvia::detail {

class client_tls_socket;

// Completion of a socket write in flight: one that waits for the reactor or
// ended inside a record. Retires it from its socket before the stream continues.
template <typename handler_type>
class client_tls_write_handler final {
public:
    client_tls_write_handler(handler_type&& handler, client_tls_socket& socket)
        : handler_(std::move(handler)),
          socket_(&socket) {}

    void operator()(const std::error_code& error, std::size_t bytes);

    // Asio's associators and continuation hook forward to the wrapped handler.
    handler_type handler_;

private:
    client_tls_socket* socket_;
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
    using lowest_layer_type = asio::ip::tcp::socket::lowest_layer_type;

    explicit client_tls_socket(const executor_type& executor)
        : socket_(executor) {}

    [[nodiscard]] executor_type get_executor() noexcept {
        return socket_.get_executor();
    }
    [[nodiscard]] lowest_layer_type& lowest_layer() noexcept {
        return socket_.lowest_layer();
    }
    [[nodiscard]] const lowest_layer_type& lowest_layer() const noexcept {
        return socket_.lowest_layer();
    }
    // The raw socket, for plaintext connections, socket options and the
    // CONNECT tunnel, which writes its own TLS records.
    [[nodiscard]] asio::ip::tcp::socket& socket() noexcept {
        return socket_;
    }
    // Every record the stream handed to the socket is whole on the wire, in
    // sequence: no write is pending or partly done, and none failed.
    [[nodiscard]] bool records_whole() const noexcept {
        return writes_ == 0 && !write_failed_;
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
        socket_.async_read_some(buffers, std::forward<handler_type>(handler));
    }
    template <typename buffers_type, typename handler_type>
    void async_write_some(const buffers_type& buffers, handler_type&& handler) {
        using completion_type = client_tls_write_handler<std::decay_t<handler_type>>;
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
                } else if (written != asio::buffer_size(buffers)) {
                    ++writes_;
                    asio::post(socket_.get_executor(),
                        asio::append(completion_type(std::forward<handler_type>(handler), *this), error, written));
                    return;
                }
                asio::post(socket_.get_executor(), asio::append(std::forward<handler_type>(handler), error, written));
                return;
            }
        }
        ++writes_;
        socket_.async_write_some(buffers, completion_type(std::forward<handler_type>(handler), *this));
    }

private:
    template <typename>
    friend class client_tls_write_handler;

    asio::ip::tcp::socket socket_;
    std::size_t writes_{0};
    // A failed or cancelled write may leave a record partly sent, or records
    // unsent whose sequence numbers a later record would skip.
    bool write_failed_{false};
};

template <typename handler_type>
void client_tls_write_handler<handler_type>::operator()(const std::error_code& error, std::size_t bytes) {
    --socket_->writes_;
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
