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
namespace ruvia::detail {
// The tunnel over the HTTP/1 connection's byte stream (plain TCP socket or TLS
// stream); a TLS stream writes its ciphertext through tls_output.
template <typename stream_type>
class http_tunnel_session final {
public:
    http_tunnel_session(stream_type& stream, tls_tunnel_output* tls_output, const worker_handle& worker_value,
        std::pmr::memory_resource& resource, std::string_view pending = {})
        : transport_(stream, tls_output),
          resource_(resource),
          pending_(pending, &resource),
          facade_(resource, worker_value, this, &read_thunk, &write_thunk, &finish_thunk, &abort_thunk) {}
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
            std::pmr::string discarded(&resource_);
            const auto read = co_await transport_.read_more(discarded);
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
    static task<std::optional<std::pmr::string>> read_thunk(void* raw) {
        auto& owner_value = *static_cast<http_tunnel_session*>(raw);
        if (!owner_value.pending_.empty()) {
            co_return std::pmr::string(std::move(owner_value.pending_), &owner_value.resource_);
        }
        std::pmr::string bytes_value(&owner_value.resource_);
        const auto result_value = co_await owner_value.transport_.read_more(bytes_value);
        if (const auto* failed = result_value.failure()) {
            throw inbound_transport_error(failed->error_code(), "HTTP tunnel read");
        }
        if (result_value.end() != nullptr) {
            owner_value.receive_ended_ = true;
            co_return std::nullopt;
        }
        co_return std::move(bytes_value);
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
    http_socket_tunnel_transport<stream_type> transport_;
    std::pmr::memory_resource& resource_;
    std::pmr::string pending_;
    bool ended_{false};
    bool receive_ended_{false};
    bool aborted_{false};
    http_tunnel facade_;
};

template <typename stream_type>
[[nodiscard]] task<void> invoke_tunnel_handler(http_tunnel_session<stream_type>& session_value,
    connection_scanner::entry_type& entry_value, const route_stream_handler_type& handler, context& context_value) {
    context_tunnel_binding binding(context_value, session_value.tunnel());
    // A draining worker closed the socket instead of admitting the tunnel.
    if (!entry_value.enter_long_lived()) {
        co_return;
    }
    co_await handler(context_value);
}

template <typename stream_type>
[[nodiscard]] task<void> finish_tunnel_session(http_tunnel_session<stream_type>& session_value,
    std::exception_ptr failure, const connection_failure_sink& sink_value, std::string_view remote,
    connection_scanner::entry_type& scanner, std::chrono::milliseconds timeout) {
    if (failure != nullptr) {
        session_value.abort();
    }
    co_await session_value.join();
    if (failure == nullptr && !session_value.aborted()) {
        struct drain_deadline final {
            http_tunnel_session<stream_type>& session_;
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
        const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
                             .count();
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
