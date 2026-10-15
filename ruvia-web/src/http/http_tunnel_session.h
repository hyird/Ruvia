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
#include "http/http_socket_tunnel_transport.h"
#include "http/http_stream_read_result.h"
#include "http/tls_tunnel_output.h"
#include "server/connection_scanner.h"
#include "server/http_server_options.h"
#include "server/inbound_transport_error.h"
#include "util/steady_clock.h"
namespace ruvia::detail {
// The tunnel over the HTTP/1 connection's byte stream (plain TCP socket or TLS
// stream); a TLS stream writes its ciphertext through tls_output. Received
// bytes live in `inbound`, the connection's inbound buffer budget: one reused
// read window, and each chunk handed to the handler, sized to the bytes read.
// `resource` holds what the handler sends and the facade's own state.
template <typename transport_type>
class http_tunnel_session final {
public:
    http_tunnel_session(transport_type transport, const worker_handle& worker_value,
        std::pmr::memory_resource& resource, std::pmr::memory_resource& inbound, std::string_view pending = {})
        : transport_(std::move(transport)),
          inbound_(inbound),
          pending_(pending, &inbound),
          read_buffer_(&inbound),
          facade_(resource, worker_value, this, &read_thunk, &write_thunk, &finish_thunk, &abort_thunk) {
        if constexpr (requires(transport_type& value) { value.read_datagram_input(); value.datagram_config(); value.send_datagram(std::span<const std::byte>{}); }) {
            facade_.read_datagram_input_ = [](void* target) -> task<std::optional<http_datagram_input>> {
                auto& session = *static_cast<http_tunnel_session*>(target);
                auto input = co_await session.transport_.read_datagram_input();
                if (!input) {
                    session.receive_ended_ = true;
                }
                co_return input;
            };
            facade_.send_datagram_ = [](void* target, std::span<const std::byte> bytes) {
                auto& session = *static_cast<http_tunnel_session*>(target);
                if (session.ended_) {
                    throw std::logic_error("HTTP Datagram send direction has ended");
                }
                session.transport_.send_datagram(bytes);
            };
            facade_.datagram_config_ = [](void* target) { return static_cast<http_tunnel_session*>(target)->transport_.datagram_config(); };
        }
    }
    [[nodiscard]] http_tunnel& tunnel() noexcept {
        return facade_;
    }
    void abort() noexcept {
        aborted_ = true;
        transport_.abort();
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
            const auto read = co_await read_some();
            if (const auto* failed = read.failure()) {
                throw inbound_transport_error(failed->error_code(), "HTTP tunnel drain");
            }
            receive_ended_ = read.end() != nullptr;
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
    // Reads into the reused window, whose capacity is allocated by the first
    // read only; the next read overwrites it.
    [[nodiscard]] task<http_stream_read_result> read_some() {
        read_buffer_.clear();
        return transport_.read_more(read_buffer_);
    }
    static task<std::optional<std::pmr::string>> read_thunk(void* raw) {
        auto& owner_value = *static_cast<http_tunnel_session*>(raw);
        if (!owner_value.pending_.empty()) {
            co_return std::exchange(owner_value.pending_, std::pmr::string(&owner_value.inbound_));
        }
        const auto result_value = co_await owner_value.read_some();
        if (const auto* failed = result_value.failure()) {
            throw inbound_transport_error(failed->error_code(), "HTTP tunnel read");
        }
        if (result_value.end() != nullptr) {
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
    static task<void> finish_thunk(void* raw) {
        return static_cast<http_tunnel_session*>(raw)->finish();
    }
    static void abort_thunk(void* raw) noexcept {
        static_cast<http_tunnel_session*>(raw)->abort();
    }
    transport_type transport_;
    std::pmr::memory_resource& inbound_;
    std::pmr::string pending_;
    std::pmr::string read_buffer_;
    bool ended_{false};
    bool receive_ended_{false};
    bool aborted_{false};
    http_tunnel facade_;
};

template <typename transport_type>
[[nodiscard]] task<void> invoke_tunnel_handler(http_tunnel_session<transport_type>& session_value,
    connection_scanner::entry_type& entry_value, const route_stream_handler_type& handler, context& context_value) {
    context_tunnel_binding binding(context_value, session_value.tunnel());
    // A draining worker closed the socket instead of admitting the tunnel.
    if (!entry_value.enter_long_lived()) {
        co_return;
    }
    co_await handler(context_value);
}

template <typename transport_type>
[[nodiscard]] task<void> finish_tunnel_session(http_tunnel_session<transport_type>& session_value,
    std::exception_ptr failure, const connection_failure_sink& sink_value, std::string_view remote,
    connection_scanner::entry_type& scanner, std::chrono::milliseconds timeout) {
    if (failure != nullptr) {
        session_value.abort();
    }
    co_await session_value.join();
    if (failure == nullptr && !session_value.aborted()) {
        struct drain_deadline final {
            http_tunnel_session<transport_type>& session_;
            std::int64_t deadline_;
            connection_scanner::periodic_check_registration_type registration_;
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
