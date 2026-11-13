#pragma once
#include <chrono>
#include <exception>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include <asio/cancellation_signal.hpp>

#include "ruvia/web/error.h"
#include "ruvia/web/http_tunnel.h"

#include "context/context_access.h"
#include "http/http_stream_end.h"
#include "integration/operation_interrupt.h"
#include "server/connection_scanner.h"
#include "server/http_server_options.h"
#include "server/inbound_buffer_budget.h"
#include "server/inbound_transport_error.h"

namespace ruvia::detail {
// A tunnel over an HTTP/1 socket transport or one HTTP/2 or HTTP/3 stream. Received
// chunks charge the connection's and worker's inbound buffer budgets.
template <typename transport_type>
class http_tunnel_session final {
public:
    http_tunnel_session(transport_type transport, const worker_ref& worker_value,
        inbound_buffer_account* inbound, bool datagrams, std::string_view pending = {})
        : transport_(std::move(transport)),
          inbound_(inbound),
          read_reservation_(inbound),
          pending_reservation_(inbound),
          delivered_reservation_(inbound),
          facade_(inbound, worker_value, this, &read_thunk, &write_thunk, &finish_thunk, &abort_thunk, datagrams) {
        if (!pending.empty()) {
            inbound_append(pending_, pending_reservation_, pending);
        }
        if constexpr (requires(transport_type& value, std::string_view bytes) {
                          value.read_datagram([] {}, asio::cancellation_slot{});
                          value.write_datagram(bytes);
                      }) {
            facade_.read_datagram_ = &read_datagram_thunk;
            facade_.write_datagram_ = &write_datagram_thunk;
        }
    }
    http_tunnel_session(transport_type, worker_ref&&, inbound_buffer_account*, bool, std::string_view = {}) = delete;
    [[nodiscard]] http_tunnel& tunnel() noexcept {
        return facade_;
    }
    void abort(bool malformed = false) noexcept {
        aborted_ = true;
        transport_.abort(malformed);
    }
    [[nodiscard]] task<void> finish(operation_options operation = {}) {
        if (!ended_) {
            operation_interrupt interrupt(interrupts_, facade_.worker_, operation, this, &abort_thunk_default, "HTTP tunnel finish");
            const auto error = co_await transport_.write_bytes({}, http_stream_end::end);
            interrupt.throw_if_interrupted();
            if (error) {
                throw inbound_transport_error(error, "HTTP tunnel finish");
            }
            ended_ = true;
        }
    }
    [[nodiscard]] bool aborted() const noexcept {
        return aborted_;
    }
    [[nodiscard]] task<void> drain() {
        delivered_reservation_.clear();
        while (!receive_ended_) {
            receive_ended_ = !(co_await read_some());
        }
    }
    [[nodiscard]] task<void> join() {
        if (facade_.read_active_ || facade_.write_active_) {
            abort();
        }
        co_await facade_.operations_.close_and_join();
    }

private:
    [[nodiscard]] task<bool> read_some() {
        if (!pending_.empty()) {
            inbound_discard(pending_, pending_reservation_);
            co_return true;
        }
        read_buffer_.clear();
        if constexpr (requires { transport_.read_more(read_buffer_, &read_reservation_); }) {
            co_return co_await transport_.read_more(read_buffer_, &read_reservation_);
        } else {
            co_return co_await transport_.read_more(read_buffer_);
        }
    }
    static void wake_read(void* raw) noexcept {
        static_cast<asio::cancellation_signal*>(raw)->emit(asio::cancellation_type::terminal);
    }
    static task<std::optional<std::string>> read_thunk(void* raw, operation_options operation) {
        auto& owner_value = *static_cast<http_tunnel_session*>(raw);
        owner_value.delivered_reservation_.clear();
        operation_interrupt interrupt(owner_value.interrupts_, owner_value.facade_.worker_, operation,
            &owner_value.read_cancellation_, &wake_read, "HTTP tunnel read");
        if (!owner_value.pending_.empty()) {
            owner_value.delivered_reservation_.swap(owner_value.pending_reservation_);
            co_return std::exchange(owner_value.pending_, std::string{});
        }
        if (owner_value.receive_ended_) {
            co_return std::nullopt;
        }
        owner_value.read_buffer_.clear();
        bool has_more = false;
        if constexpr (requires(transport_type& t, std::string& b, asio::cancellation_slot sl, inbound_buffer_reservation* r) { t.read_more(b, [] {}, sl, r); }) {
            has_more = co_await owner_value.transport_.read_more(
                owner_value.read_buffer_, [&interrupt] { interrupt.throw_if_interrupted(); },
                owner_value.read_cancellation_.slot(), &owner_value.read_reservation_);
        } else {
            has_more = co_await owner_value.transport_.read_more(
                owner_value.read_buffer_, [&interrupt] { interrupt.throw_if_interrupted(); },
                owner_value.read_cancellation_.slot());
        }
        if (!has_more) {
            owner_value.receive_ended_ = true;
            co_return std::nullopt;
        }
        std::string chunk;
        inbound_append(chunk, owner_value.delivered_reservation_, owner_value.read_buffer_);
        co_return chunk;
    }
    static task<void> write_thunk(void* raw, std::string_view bytes_value, operation_options operation) {
        auto& owner_value = *static_cast<http_tunnel_session*>(raw);
        if (owner_value.ended_) {
            throw std::logic_error("HTTP tunnel send direction has ended");
        }
        operation_interrupt interrupt(owner_value.interrupts_, owner_value.facade_.worker_, operation, raw,
            &abort_thunk_default, "HTTP tunnel write");
        const auto error = co_await owner_value.transport_.write_bytes(bytes_value, http_stream_end::keep_open);
        interrupt.throw_if_interrupted();
        if (error) {
            throw inbound_transport_error(error, "HTTP tunnel write");
        }
    }
    static task<std::optional<http_tunnel_input>> read_datagram_thunk(void* raw, operation_options operation) {
        auto& owner_value = *static_cast<http_tunnel_session*>(raw);
        owner_value.delivered_reservation_.clear();
        operation_interrupt interrupt(owner_value.interrupts_, owner_value.facade_.worker_, operation,
            &owner_value.read_cancellation_, &wake_read, "HTTP tunnel read");
        if (!owner_value.pending_.empty()) {
            owner_value.delivered_reservation_.swap(owner_value.pending_reservation_);
            co_return http_tunnel_input{std::exchange(owner_value.pending_, std::string{}), false};
        }
        if (owner_value.receive_ended_) {
            co_return std::nullopt;
        }
        auto input = co_await owner_value.transport_.read_datagram(
            [&interrupt] { interrupt.throw_if_interrupted(); }, owner_value.read_cancellation_.slot());
        if (!input) {
            owner_value.receive_ended_ = true;
        }
        co_return input;
    }
    static task<bool> write_datagram_thunk(void* raw, std::string_view bytes, operation_options operation) {
        auto& owner_value = *static_cast<http_tunnel_session*>(raw);
        if (owner_value.ended_) {
            throw std::logic_error("HTTP tunnel send direction has ended");
        }
        operation_interrupt interrupt(owner_value.interrupts_, owner_value.facade_.worker_, operation, raw,
            &abort_thunk_default, "HTTP tunnel write");
        try {
            const bool sent = co_await owner_value.transport_.write_datagram(bytes);
            interrupt.throw_if_interrupted();
            co_return sent;
        } catch (...) {
            interrupt.throw_if_interrupted();
            throw;
        }
    }
    static task<void> finish_thunk(void* raw, operation_options operation) {
        return static_cast<http_tunnel_session*>(raw)->finish(std::move(operation));
    }
    static void abort_thunk(void* raw, bool malformed) noexcept {
        static_cast<http_tunnel_session*>(raw)->abort(malformed);
    }
    static void abort_thunk_default(void* raw) noexcept {
        static_cast<http_tunnel_session*>(raw)->abort();
    }
    transport_type transport_;
    inbound_buffer_account* inbound_;
    std::string read_buffer_;
    inbound_buffer_reservation read_reservation_;
    std::string pending_;
    inbound_buffer_reservation pending_reservation_;
    inbound_buffer_reservation delivered_reservation_;
    bool ended_{false};
    bool receive_ended_{false};
    bool aborted_{false};
    asio::cancellation_signal read_cancellation_;
    operation_interrupt_source interrupts_;
    http_tunnel facade_;
};

template <typename transport_type>
[[nodiscard]] task<void> invoke_tunnel_handler(http_tunnel_session<transport_type>& session_value,
    connection_scanner::entry& entry_value, const route_stream_handler& handler, context& context_value) {
    context_tunnel_binding binding(context_value, session_value.tunnel());
    entry_value.enter_long_lived();
    co_await handler(context_value);
}

template <typename transport_type>
[[nodiscard]] task<void> finish_tunnel_session(http_tunnel_session<transport_type>& session_value,
    std::exception_ptr failure, const connection_failure_sink& sink_value, std::string_view remote,
    connection_scanner::entry& scanner, timeout_duration timeout) {
    if (failure != nullptr) {
        session_value.abort();
    } else if (!session_value.aborted()) {
        struct drain_deadline final {
            http_tunnel_session<transport_type>& session_;
            std::int64_t deadline_;
            connection_scanner::periodic_check_registration registration_;
            static void tick(void* raw, std::int64_t now) noexcept {
                auto& self = *static_cast<drain_deadline*>(raw);
                if (now >= self.deadline_) {
                    self.session_.abort();
                }
            }
        } deadline{session_value, steady_now_ms() + timeout.value().count(), {}};
        scanner.register_periodic_check(deadline.registration_, &deadline, &drain_deadline::tick);
        try {
            co_await session_value.finish();
            co_await session_value.drain();
        } catch (...) {
            session_value.abort();
        }
    }
    co_await session_value.join();
    if (failure != nullptr) {
        sink_value.invoke(remote, failure);
        std::rethrow_exception(failure);
    }
}
}  // namespace ruvia::detail
