#pragma once

#include <span>
#include <type_traits>
#include <utility>

#include <asio/associated_allocator.hpp>
#include <asio/associated_cancellation_slot.hpp>
#include <asio/associated_executor.hpp>
#include <asio/buffer.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/post.hpp>

#include "server/connection_lifecycle.h"

namespace ruvia::detail {

// The caller owns the socket and the already-read prefix until all I/O joins.
// Prefix delivery is asynchronous, like socket reads, and never reads a byte twice.
class inbound_socket_stream final {
public:
    using executor_type = asio::ip::tcp::socket::executor_type;
    using lowest_layer_type = asio::ip::tcp::socket;

    explicit inbound_socket_stream(asio::ip::tcp::socket& socket, std::span<const std::byte> prefix = {},
        connection_lifecycle* lifecycle = nullptr) noexcept
        : socket_(socket),
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
            socket_.async_read_some(buffers, completion<std::decay_t<handler_type>, false>{
                                                 std::forward<handler_type>(handler), socket_.get_executor(), lifecycle_});
            return;
        }
        const auto bytes = consume_prefix(buffers);
        asio::post(socket_.get_executor(), [handler = std::forward<handler_type>(handler), bytes]() mutable {
            std::move(handler)(std::error_code{}, bytes);
        });
    }

    template <typename buffers_type, typename handler_type>
    void async_write_some(const buffers_type& buffers, handler_type&& handler) {
        socket_.async_write_some(buffers, completion<std::decay_t<handler_type>, true>{
                                              std::forward<handler_type>(handler), socket_.get_executor(), lifecycle_});
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

}  // namespace ruvia::detail
