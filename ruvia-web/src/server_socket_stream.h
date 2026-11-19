#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <span>
#include <system_error>
#include <type_traits>
#include <utility>

#include <asio/associator.hpp>
#include <asio/bind_cancellation_slot.hpp>
#include <asio/buffer.hpp>
#include <asio/cancellation_signal.hpp>
#include <asio/error.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/post.hpp>
#include <asio/require.hpp>
#include <asio/ssl/stream.hpp>

#include "connection_lifecycle.h"
#include "connection_scanner.h"
#include "task.h"
#include "worker_ref.h"

// The accepted server socket as a stream: prefix replay, lifecycle byte
// accounting and native write progress, bounded inline completion of its
// writes, its TLS transport, and how a TCP connection session closes.
namespace ruvia::detail {

class inbound_socket_stream;
template <typename handler_type, bool writing>
struct inbound_socket_completion;

// The immediate executor (Asio associated_immediate_executor) bound to the
// connection's gather writes. An operation completing within its initiation
// continues inline on the owner worker while the stream's cooperative budget
// and this thread's nesting bound allow; otherwise its completion is posted to
// the worker queue, as Asio's default immediate completion is. It refers to
// its stream, which outlives every operation that can complete through it.
class inbound_immediate_executor final {
public:
    explicit inbound_immediate_executor(inbound_socket_stream& stream) noexcept :
        stream_(&stream) {}

    template <typename function_type>
    void execute(function_type&& function) const;

    // Whether this thread is inside an inline completion rather than one the
    // event loop delivered.
    [[nodiscard]] static bool completing_inline() noexcept {
        return depth_ != 0;
    }

    friend bool operator==(const inbound_immediate_executor&, const inbound_immediate_executor&) noexcept = default;

private:
    class nesting final {
    public:
        nesting() noexcept {
            ++depth_;
        }
        ~nesting() {
            --depth_;
        }
        nesting(const nesting&) = delete;
        nesting& operator=(const nesting&) = delete;
    };

    // An inline completion stays on the stack of its initiation, and a TLS
    // write initiates its next record from there, so nesting is bounded.
    static constexpr std::uint8_t max_depth = 8;
    // Inline completions running on this thread. Kept per thread, not per
    // stream, because it is restored after a completion that may have ended,
    // and destroyed, the stream.
    static inline thread_local std::uint8_t depth_{};

    inbound_socket_stream* stream_;
};

// The caller owns the socket and the already-read prefix until all I/O joins.
// Prefix delivery is asynchronous, like socket reads, and never reads a byte twice.
// Its address stays fixed until pending I/O joins.
// Every socket transfer reports to `lifecycle`; a sent byte is write progress of
// its bound scanner entry, so TLS output renews write_timeout per ciphertext
// flush rather than per plaintext write.
// The socket belongs to its worker's io_context.
class inbound_socket_stream final {
public:
    using executor_type = asio::ip::tcp::socket::executor_type;
    using lowest_layer_type = asio::ip::tcp::socket;

    explicit inbound_socket_stream(asio::ip::tcp::socket& socket, std::span<const std::byte> prefix = {}, connection_lifecycle* lifecycle = nullptr) noexcept :
        socket_(socket),
        io_executor_(worker_io_executor(socket)),
        prefix_(prefix),
        lifecycle_(lifecycle) {}

    inbound_socket_stream(const inbound_socket_stream&) = delete;
    inbound_socket_stream& operator=(const inbound_socket_stream&) = delete;

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
    [[nodiscard]] bool inline_write_available() const noexcept {
        return remaining_inline_writes_ != 0;
    }
    [[nodiscard]] inbound_immediate_executor immediate_executor() noexcept {
        return inbound_immediate_executor(*this);
    }
    void shutdown(asio::ip::tcp::socket::shutdown_type what, std::error_code& error) {
        socket_.shutdown(what, error);
    }
    template <typename buffers_type>
    std::size_t send(const buffers_type& buffers, asio::socket_base::message_flags flags, std::error_code& error) {
#if defined(_WIN32)
        // Windows Winsock accepts an arbitrarily large single WSASend whenever
        // currently buffered bytes are below SO_SNDBUF; slice transfers to
        // preserve socket backpressure and write timeouts.
        const auto bytes = [&] {
            if constexpr (std::is_convertible_v<const buffers_type&, asio::const_buffer>) {
                return socket_.send(asio::buffer(asio::const_buffer(buffers), max_native_write_bytes), flags, error);
            } else {
                const bounded_const_buffers bounded(buffers);
                return socket_.send(bounded, flags, error);
            }
        }();
#else
        const auto bytes = socket_.send(buffers, flags, error);
#endif
        if (bytes != 0 && remaining_inline_writes_ != 0) {
            --remaining_inline_writes_;
        }
        if (lifecycle_ != nullptr) {
            lifecycle_->note_transport_write(bytes, error);
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
            socket_.async_read_some(buffers, inbound_socket_completion<std::decay_t<handler_type>, false>{std::forward<handler_type>(handler), this});
            return;
        }
        const auto bytes = consume_prefix(buffers);
        asio::post(socket_.get_executor(), [handler = std::forward<handler_type>(handler), bytes]() mutable {
            std::move(handler)(std::error_code{}, bytes);
        });
    }

    template <typename buffers_type, typename handler_type>
    void async_write_some(const buffers_type& buffers, handler_type&& handler) {
        inbound_socket_completion<std::decay_t<handler_type>, true> completion{std::forward<handler_type>(handler), this};
#if defined(_WIN32)
        if constexpr (std::is_convertible_v<const buffers_type&, asio::const_buffer>) {
            socket_.async_write_some(asio::buffer(asio::const_buffer(buffers), max_native_write_bytes), std::move(completion));
        } else {
            const bounded_const_buffers bounded(buffers);
            socket_.async_write_some(bounded, std::move(completion));
        }
#else
        socket_.async_write_some(buffers, std::move(completion));
#endif
    }

private:
    friend class inbound_immediate_executor;
    template <typename, bool>
    friend struct inbound_socket_completion;

#if defined(_WIN32)
    static constexpr std::size_t max_native_write_bytes = std::size_t{64} * 1024;

    struct bounded_const_buffers final {
        using value_type = asio::const_buffer;
        using const_iterator = const asio::const_buffer*;

        std::array<asio::const_buffer, 16> pieces_{};
        std::size_t count_{0};

        template <typename buffers_type>
        explicit bounded_const_buffers(const buffers_type& buffers) noexcept {
            std::size_t remaining = max_native_write_bytes;
            auto current = asio::buffer_sequence_begin(buffers);
            const auto end = asio::buffer_sequence_end(buffers);
            for (; current != end && remaining != 0 && count_ != pieces_.size(); ++current) {
                const asio::const_buffer piece(*current);
                if (piece.size() == 0) {
                    continue;
                }
                const auto sliced = asio::buffer(piece, remaining);
                remaining -= sliced.size();
                pieces_[count_++] = sliced;
            }
        }

        [[nodiscard]] const_iterator begin() const noexcept {
            return pieces_.data();
        }
        [[nodiscard]] const_iterator end() const noexcept {
            return pieces_.data() + count_;
        }
    };
#endif

    [[nodiscard]] static asio::io_context::executor_type worker_io_executor(asio::ip::tcp::socket& socket) noexcept {
        const auto executor = socket.get_executor();
        const auto* io_executor = executor.target<asio::io_context::executor_type>();
        if (io_executor == nullptr) {
            std::terminate();
        }
        return *io_executor;
    }
    template <typename buffers_type>
    std::size_t consume_prefix(const buffers_type& buffers) noexcept {
        const auto bytes = asio::buffer_copy(buffers, asio::buffer(prefix_.data(), prefix_.size()));
        prefix_ = prefix_.subspan(bytes);
        return bytes;
    }
    asio::ip::tcp::socket& socket_;
    asio::io_context::executor_type io_executor_;
    std::span<const std::byte> prefix_;
    connection_lifecycle* lifecycle_;
    // A hot producer must yield through the event loop periodically. Each
    // non-blocking send and each inline completion spends one; a completion
    // the event loop delivers refills it. Consecutive writes share it.
    static constexpr std::uint8_t max_inline_writes = 128;
    std::uint8_t remaining_inline_writes_{max_inline_writes};
};

// Reports one transfer of `owner_` before its handler runs. Asio's
// associators forward to the wrapped handler (see below): a write bound to
// inbound_immediate_executor may complete inline, while any other operation
// keeps Asio's default immediate completion through the worker queue.
template <typename handler_type, bool writing>
struct inbound_socket_completion final {
    handler_type handler_;
    inbound_socket_stream* owner_;

    void operator()(const std::error_code& error, std::size_t bytes) {
        // An inline completion already spent its unit in the executor.
        if (!inbound_immediate_executor::completing_inline()) {
            owner_->remaining_inline_writes_ = inbound_socket_stream::max_inline_writes;
        }
        auto* lifecycle = owner_->lifecycle_;
        if (lifecycle != nullptr) {
            if constexpr (writing) {
                lifecycle->note_transport_write(bytes, error);
            } else {
                lifecycle->note_read(bytes);
            }
            // Protocol drivers also cancel reads for a successful drain.
            // Timeout, shutdown and explicit abort record their cause first.
            if (error != asio::error::operation_aborted && error != asio::error::bad_descriptor) {
                lifecycle->note_error(error);
            }
        }
        std::move(handler_)(error, bytes);
    }
};

// Runs within the initiation of an operation on the stream, so the stream is
// alive on entry. The completion may end the stream's owner: after it, only
// this thread's nesting depth is touched.
template <typename function_type>
void inbound_immediate_executor::execute(function_type&& function) const {
    auto& stream = *stream_;
    if (depth_ < max_depth && stream.io_executor_.running_in_this_thread() && stream.remaining_inline_writes_ != 0) {
        --stream.remaining_inline_writes_;
        const nesting scope;
        std::forward<function_type>(function)();
        return;
    }
    asio::require(stream.io_executor_, asio::execution::blocking.never).execute(std::forward<function_type>(function));
}

}  // namespace ruvia::detail

namespace asio {

template <template <typename, typename> class associator_type, typename handler_type, bool writing, typename candidate_type>
struct associator<associator_type, ruvia::detail::inbound_socket_completion<handler_type, writing>, candidate_type> : associator_type<handler_type, candidate_type> {
    static typename associator_type<handler_type, candidate_type>::type get(const ruvia::detail::inbound_socket_completion<handler_type, writing>& completion) noexcept {
        return associator_type<handler_type, candidate_type>::get(completion.handler_);
    }
    static auto get(const ruvia::detail::inbound_socket_completion<handler_type, writing>& completion, const candidate_type& candidate) noexcept -> decltype(associator_type<handler_type, candidate_type>::get(completion.handler_, candidate)) {
        return associator_type<handler_type, candidate_type>::get(completion.handler_, candidate);
    }
};

}  // namespace asio

namespace ruvia::detail {

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

    [[nodiscard]] inbound_immediate_executor immediate_executor() noexcept {
        return socket_.immediate_executor();
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
