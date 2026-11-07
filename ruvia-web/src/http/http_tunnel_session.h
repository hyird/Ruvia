#pragma once
#include <chrono>
#include <exception>
#include <limits>
#include <memory_resource>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include "ruvia/web/detail/router/route_handlers.h"
#include "ruvia/web/http_tunnel.h"

#include "context/context_access.h"
#include "server/connection_scanner.h"
#include "server/http_server_options.h"
#include "server/inbound_transport_error.h"
#include "tls/tls_stream_end.h"
#include "util/steady_clock.h"
namespace ruvia::detail {
// A tunnel over an HTTP/1 socket transport or one HTTP/2 or HTTP/3 stream. Received
// chunks use the connection's inbound budget; sent data and facade state
// use the worker resource. The transport owns protocol-specific flow control.
template <typename transport_type>
class http_tunnel_session final {
public:
    http_tunnel_session(transport_type transport, const worker_handle& worker_value,
        std::pmr::memory_resource& resource, std::pmr::memory_resource& inbound, bool datagrams,
        std::string_view pending = {})
        : transport_(std::move(transport)),
          inbound_(inbound),
          read_buffer_(&inbound),
          pending_(pending, &inbound),
          facade_(resource, worker_value, this, &read_thunk, &write_thunk, &finish_thunk, &abort_thunk, datagrams) {
        if constexpr (requires(transport_type& value, std::string_view bytes) {
                          value.read_datagram();
                          value.write_datagram(bytes);
                      }) {
            facade_.read_datagram_ = &read_datagram_thunk;
            facade_.write_datagram_ = &write_datagram_thunk;
        }
    }
    http_tunnel_session(transport_type, worker_handle&&, std::pmr::memory_resource&,
        std::pmr::memory_resource&, bool, std::string_view = {}) = delete;
    [[nodiscard]] http_tunnel& tunnel() noexcept {
        return facade_;
    }
    void abort(bool malformed = false) noexcept {
        aborted_ = true;
        transport_.abort(malformed);
    }
    [[nodiscard]] task<void> finish() {
        if (!ended_) {
            const auto error = co_await transport_.write_bytes({}, http_stream_end::end);
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
        while (!receive_ended_) {
            receive_ended_ = !(co_await read_some());
        }
    }
    // The transport must be stopped before joining if an operation remains
    // suspended. Cold operations expire while their borrowed owner is alive.
    [[nodiscard]] task<void> join() {
        if (facade_.running_operations_ != 0) {
            abort();
        }
        co_await facade_.operations_.close_and_join();
    }

private:
    [[nodiscard]] task<bool> read_some() {
        if (!pending_.empty()) {
            pending_ = std::pmr::string(&inbound_);
            co_return true;
        }
        read_buffer_.clear();
        co_return co_await transport_.read_more(read_buffer_);
    }
    static task<std::optional<std::pmr::string>> read_thunk(void* raw) {
        auto& owner_value = *static_cast<http_tunnel_session*>(raw);
        if (!owner_value.pending_.empty()) {
            co_return std::exchange(owner_value.pending_, std::pmr::string(&owner_value.inbound_));
        }
        if (owner_value.receive_ended_) {
            co_return std::nullopt;
        }
        if (!(co_await owner_value.read_some())) {
            owner_value.receive_ended_ = true;
            co_return std::nullopt;
        }
        co_return std::pmr::string(owner_value.read_buffer_, &owner_value.inbound_);
    }
    static task<void> write_thunk(void* raw, std::string_view bytes_value) {
        auto& owner_value = *static_cast<http_tunnel_session*>(raw);
        if (owner_value.ended_) {
            throw std::logic_error("HTTP tunnel send direction has ended");
        }
        const auto error = co_await owner_value.transport_.write_bytes(bytes_value, http_stream_end::keep_open);
        if (error) {
            throw inbound_transport_error(error, "HTTP tunnel write");
        }
    }
    static task<std::optional<http_tunnel_input>> read_datagram_thunk(void* raw) {
        auto& owner_value = *static_cast<http_tunnel_session*>(raw);
        if (!owner_value.pending_.empty()) {
            co_return http_tunnel_input{std::exchange(owner_value.pending_, std::pmr::string(&owner_value.inbound_)), false};
        }
        if (owner_value.receive_ended_) {
            co_return std::nullopt;
        }
        auto input = co_await owner_value.transport_.read_datagram();
        if (!input) {
            owner_value.receive_ended_ = true;
        }
        co_return input;
    }
    static task<bool> write_datagram_thunk(void* raw, std::string_view bytes) {
        auto& owner_value = *static_cast<http_tunnel_session*>(raw);
        if (owner_value.ended_) {
            throw std::logic_error("HTTP tunnel send direction has ended");
        }
        co_return co_await owner_value.transport_.write_datagram(bytes);
    }
    static task<void> finish_thunk(void* raw) {
        return static_cast<http_tunnel_session*>(raw)->finish();
    }
    static void abort_thunk(void* raw, bool malformed) noexcept {
        static_cast<http_tunnel_session*>(raw)->abort(malformed);
    }
    transport_type transport_;
    std::pmr::memory_resource& inbound_;
    std::pmr::string read_buffer_;
    std::pmr::string pending_;
    bool ended_{false};
    bool receive_ended_{false};
    bool aborted_{false};
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
    connection_scanner::entry& scanner, std::chrono::milliseconds timeout) {
    if (failure != nullptr) {
        session_value.abort();
    }
    co_await session_value.join();
    if (failure == nullptr && !session_value.aborted()) {
        struct drain_deadline final {
            http_tunnel_session<transport_type>& session_;
            std::int64_t deadline_;
            connection_scanner::periodic_check_registration registration_;
            static void tick(void* raw, std::int64_t now) noexcept {
                auto& owner_value = *static_cast<drain_deadline*>(raw);
                if (now >= owner_value.deadline_) {
                    owner_value.registration_.reset();
                    owner_value.session_.abort();
                }
            }
        } deadline_value{session_value, 0, {}};
        const auto now = steady_now_ms();
        deadline_value.deadline_ = timeout.count() > (std::numeric_limits<std::int64_t>::max)() - now
                                       ? (std::numeric_limits<std::int64_t>::max)()
                                       : now + timeout.count();
        scanner.register_periodic_check(deadline_value.registration_, &deadline_value, &drain_deadline::tick);
        try {
            co_await session_value.finish();
            co_await session_value.drain();
        } catch (...) {
            failure = std::current_exception();
        }
    }
    if (failure != nullptr) {
        session_value.abort();
        sink_value.invoke(remote, failure);
    }
}
}  // namespace ruvia::detail
