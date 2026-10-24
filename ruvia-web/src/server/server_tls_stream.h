#pragma once

#include <cstddef>
#include <utility>

#include <asio/append.hpp>
#include <asio/bind_cancellation_slot.hpp>
#include <asio/cancellation_signal.hpp>
#include <asio/error.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/post.hpp>
#include <asio/ssl/stream.hpp>

namespace ruvia::detail {

// SSL 的读操作也可能发送 KeyUpdate 等控制记录。取消读允许保留已接收的
// 密文字节，但不能截断已从 SSL BIO 取出的输出，否则后续复用会丢失记录。
// 屏蔽单操作的写取消；socket 关闭和既有连接超时仍能终止底层写入。
class tls_socket_transport final {
public:
    using executor_type = asio::ip::tcp::socket::executor_type;
    using lowest_layer_type = asio::ip::tcp::socket;

    explicit tls_socket_transport(asio::ip::tcp::socket& socket) noexcept
        : socket_(socket) {}

    [[nodiscard]] executor_type get_executor() noexcept {
        return socket_.get_executor();
    }

    [[nodiscard]] lowest_layer_type& lowest_layer() noexcept {
        return socket_;
    }

    [[nodiscard]] const lowest_layer_type& lowest_layer() const noexcept {
        return socket_;
    }

    template <typename buffers_type, typename handler_type>
    auto async_read_some(const buffers_type& buffers, handler_type&& handler) {
        if (peer_read_closing_ != nullptr) {
            if (*peer_read_closing_) {
                return asio::post(socket_.get_executor(), asio::append(std::forward<handler_type>(handler),
                                                              asio::error::make_error_code(asio::error::operation_aborted), std::size_t{0}));
            }
            return socket_.async_read_some(buffers,
                asio::bind_cancellation_slot(peer_read_cancellation_, std::forward<handler_type>(handler)));
        }
        return socket_.async_read_some(buffers, std::forward<handler_type>(handler));
    }

    // 监测器持有这些借用直到原生读和 TLS 控制写均已 join。取消只作用于
    // 原生读；closing 阻止处理 KeyUpdate 后重新投递读，且不截断 TLS 输出。
    void bind_peer_read_cancellation(asio::cancellation_slot slot, const bool& closing) noexcept {
        peer_read_cancellation_ = slot;
        peer_read_closing_ = &closing;
    }

    void clear_peer_read_cancellation() noexcept {
        peer_read_cancellation_ = {};
        peer_read_closing_ = nullptr;
    }

    template <typename buffers_type, typename handler_type>
    auto async_write_some(const buffers_type& buffers, handler_type&& handler) {
        return socket_.async_write_some(buffers,
            asio::bind_cancellation_slot(asio::cancellation_slot{}, std::forward<handler_type>(handler)));
    }

private:
    asio::ip::tcp::socket& socket_;
    asio::cancellation_slot peer_read_cancellation_;
    const bool* peer_read_closing_{nullptr};
};

using server_tls_stream = asio::ssl::stream<tls_socket_transport>;

}  // namespace ruvia::detail
