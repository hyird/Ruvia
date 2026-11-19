#include "server_connection.h"

#include <cstddef>
#include <exception>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include <asio/cancellation_signal.hpp>
#include <asio/ip/tcp.hpp>

#include "byte_view.h"
#include "connection_lifecycle.h"
#include "connection_scanner.h"
#include "http_tunnel_session.h"
#include "inbound_buffer.h"
#include "io_operation.h"
#include "ip_address_conversion.h"
#include "operation_scope.h"
#include "server_options.h"
#include "server_socket_stream.h"
#include "stop_token.h"
#include "task.h"
#include "transport_stream_end.h"
#include "worker_connections.h"
#include "worker_ref.h"

namespace ruvia::detail {

// The byte stream behind one server_connection: the accepted TCP socket, or
// the TLS stream whose writes leave through `tls_output`. Reads land in one
// buffer charged to the connection's inbound budget and reused for every
// read; each read and write that completes renews the scanner entry's
// activity, so idle_timeout_ measures time without I/O.
template <typename stream_type>
class server_connection_session final {
public:
    server_connection_session(stream_type& stream, tls_tunnel_output* tls_output, connection_scanner::entry& entry, inbound_buffer_account& inbound, const worker_ref& worker_value, const stop_token& stop_token_value, const server_connection_fields& fields) :
        transport_(stream, tls_output, &entry),
        entry_(entry),
        lifecycle_(fields.lifecycle_),
        worker_(worker_value),
        read_reservation_(&inbound),
        facade_(fields, stop_token_value, worker_value, this, &read_thunk, &write_thunk, &finish_thunk, &abort_thunk) {}
    server_connection_session(const server_connection_session&) = delete;
    server_connection_session& operator=(const server_connection_session&) = delete;

    [[nodiscard]] server_connection& connection() noexcept {
        return facade_;
    }
    [[nodiscard]] bool aborted() const noexcept {
        return aborted_;
    }
    void abort() noexcept {
        aborted_ = true;
        if (lifecycle_ != nullptr) {
            lifecycle_->set_reason(connection_close_reason::aborted);
        }
        transport_.abort();
    }
    // Expires the facade once the handler returned. An operation the handler
    // left running is aborted first, so the join cannot wait on the peer.
    [[nodiscard]] operation_scope::join_operation join() {
        if (facade_.read_active_ || facade_.write_active_) {
            abort();
        }
        return facade_.operations_.close_and_join();
    }
    [[nodiscard]] bool send_ended() const noexcept {
        return facade_.send_ended_;
    }
    void mark_send_ended() noexcept {
        facade_.send_ended_ = true;
    }
    [[nodiscard]] task<void> end_send(operation_options operation) {
        operation_interrupt interrupt(interrupts_, worker_, operation, this, &abort_thunk, "server connection finish");
        const auto error = co_await transport_.write_bytes({}, http_stream_end::end);
        interrupt.throw_if_interrupted();
        if (error) {
            throw inbound_transport_error(error, "server connection finish");
        }
    }

private:
    static void wake_read(void* raw) noexcept {
        static_cast<asio::cancellation_signal*>(raw)->emit(asio::cancellation_type::terminal);
    }
    static constexpr const char* lane_in_progress = "server connection operation already in progress on this direction";
    static task<std::optional<std::span<const std::byte>>> read_thunk(void* raw, bool& read_active, operation_options operation) {
        auto lease = claim_operation_lane(read_active, lane_in_progress);
        auto& owner = *static_cast<server_connection_session*>(raw);
        operation_interrupt interrupt(owner.interrupts_, owner.worker_, operation, &owner.read_cancellation_, &wake_read, "server connection read");
        if (owner.receive_ended_) {
            co_return std::nullopt;
        }
        owner.read_buffer_.clear();
        if (!(co_await owner.transport_.read_more(owner.read_buffer_, [&interrupt] {
            interrupt.throw_if_interrupted();
        }, owner.read_cancellation_.slot(), &owner.read_reservation_))) {
            owner.receive_ended_ = true;
            if (owner.lifecycle_ != nullptr) {
                owner.lifecycle_->set_reason(connection_close_reason::peer_eof);
            }
            co_return std::nullopt;
        }
        owner.entry_.touch();
        co_return std::as_bytes(std::span(owner.read_buffer_.data(), owner.read_buffer_.size()));
    }
    static task<void> write_thunk(void* raw, std::string bytes, bool& write_active, const bool& send_ended, operation_options operation) {
        auto lease = claim_operation_lane(write_active, lane_in_progress);
        if (send_ended) {
            throw std::logic_error("server connection send direction has ended");
        }
        auto& owner = *static_cast<server_connection_session*>(raw);
        operation_interrupt interrupt(owner.interrupts_, owner.worker_, operation, raw, &abort_thunk, "server connection write");
        const auto error = co_await owner.transport_.write_bytes(bytes, http_stream_end::keep_open);
        interrupt.throw_if_interrupted();
        if (error) {
            throw inbound_transport_error(error, "server connection write");
        }
    }
    static task<void> finish_thunk(void* raw, bool& write_active, bool& send_ended, operation_options operation) {
        auto lease = claim_operation_lane(write_active, lane_in_progress);
        if (!send_ended) {
            co_await static_cast<server_connection_session*>(raw)->end_send(std::move(operation));
            send_ended = true;
        }
    }
    static void abort_thunk(void* raw) noexcept {
        static_cast<server_connection_session*>(raw)->abort();
    }

    http_socket_tunnel_transport<stream_type> transport_;
    connection_scanner::entry& entry_;
    connection_lifecycle* lifecycle_;
    const worker_ref& worker_;
    std::string read_buffer_;
    inbound_buffer_reservation read_reservation_;
    asio::cancellation_signal read_cancellation_;
    bool receive_ended_{false};
    bool aborted_{false};
    operation_interrupt_source interrupts_;
    server_connection facade_;
};

}  // namespace ruvia::detail

namespace ruvia {

server_connection::server_connection(const detail::server_connection_fields& fields, const ::ruvia::stop_token& stop_token_value, const worker_ref& worker_value, void* target, read_type read, write_type write, finish_type finish_value, abort_type abort_value) noexcept :
    connection_endpoints(fields.endpoints_),
    worker_(worker_value),
    target_(target),
    read_(read),
    write_(write),
    finish_(finish_value),
    abort_(abort_value),
    tls_(fields.tls_),
    stop_token_(stop_token_value),
    state_(fields.state_) {
    operations_.bind_worker(worker_);
}

void server_connection::require_active() const {
    if (!worker_.is_current()) {
        std::terminate();
    }
    if (!operations_.active()) {
        throw std::logic_error("server connection has expired");
    }
}

task<std::optional<std::span<const std::byte>>> server_connection::read(operation_options operation) & {
    require_active();
    return operations_.attach(read_(target_, read_active_, std::move(operation)));
}

task<std::optional<std::string_view>> server_connection::read_text(operation_options operation) & {
    const auto chunk = co_await read(std::move(operation));
    if (!chunk) {
        co_return std::nullopt;
    }
    co_return as_chars(*chunk);
}

task<void> server_connection::write(outbound_bytes bytes, operation_options operation) & {
    require_active();
    if (send_ended_) {
        throw std::logic_error("server connection send direction has ended");
    }
    return operations_.attach(write_(target_, std::move(bytes).take_string(), write_active_, send_ended_, std::move(operation)));
}

task<void> server_connection::finish(operation_options operation) & {
    require_active();
    return operations_.attach(finish_(target_, write_active_, send_ended_, std::move(operation)));
}

void server_connection::abort() & noexcept {
    if (!worker_.is_current()) {
        std::terminate();
    }
    if (operations_.active()) {
        abort_(target_);
    }
}

}  // namespace ruvia

namespace ruvia::detail {

// The handler owns the connection until its task completes; the connection
// then ends here, so a handler never has to close it itself. The TLS ciphertext
// writer outlives the session that writes through it and is joined on every
// exit.
template <typename stream_type>
task<void> worker_connections::serve_connection(stream_type& stream, tcp_socket_type& socket, server_connection_fields fields) {
    // A stopping worker has already closed the connection.
    if (!http_server_worker_running(state_) || !socket.is_open()) {
        co_return;
    }
    connection_scanner::entry scanner_entry;
    scanner_entry.bind_lifecycle(fields.lifecycle_);
    connection_scanner::guard scanner_guard(scanner_, scanner_entry, connection_transport::tcp(socket));
    inbound_buffer_account connection_buffers(*options_.inbound_budget_, options_.server_.max_inbound_buffer_bytes_per_connection_);
    std::optional<tls_tunnel_output> tls_output;
    if constexpr (requires { stream.next_layer(); }) {
        tls_output.emplace(*stream.native_handle(), socket, worker_, fields.lifecycle_);
        tls_output->start();
    }
    std::exception_ptr failure;
    {
        server_connection_session<stream_type> session(stream, tls_output.has_value() ? &*tls_output : nullptr, scanner_entry, connection_buffers, worker_, stop_token_, fields);
        try {
            co_await options_.connection_(session.connection());
        } catch (...) {
            failure = std::current_exception();
            if (fields.lifecycle_ != nullptr) {
                fields.lifecycle_->note_exception(failure);
            }
        }
        if (failure != nullptr) {
            session.abort();
        }
        co_await session.join();
        // A worker stop keeps the immediate close.
        if (failure == nullptr && !session.aborted() && !stop_token_.stop_requested() && !session.send_ended()) {
            try {
                session.mark_send_ended();
                co_await session.end_send({});
            } catch (...) {
                failure = std::current_exception();
                if (fields.lifecycle_ != nullptr) {
                    fields.lifecycle_->note_exception(failure);
                }
            }
        }
    }
    if (tls_output.has_value()) {
        if (failure != nullptr) {
            tls_output->abort();
        }
        co_await tls_output->join();
    }
    if (failure != nullptr) {
        std::rethrow_exception(failure);
    }
}

template task<void> worker_connections::serve_connection<inbound_socket_stream>(inbound_socket_stream&, tcp_socket_type&, server_connection_fields);
template task<void> worker_connections::serve_connection<server_tls_stream>(server_tls_stream&, tcp_socket_type&, server_connection_fields);

}  // namespace ruvia::detail
