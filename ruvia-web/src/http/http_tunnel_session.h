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

#include "ruvia/core/connection_scanner.h"
#include "ruvia/web/detail/util/callable_ref.h"
#include "ruvia/web/http_tunnel.h"

#include "context/context_access.h"
#include "context/http_tunnel_access.h"
#include "http/http_stream_read_result.h"
#include "server/http_server_options.h"
namespace ruvia::detail {
template <typename transport_type>
class http_tunnel_session final {
public:
    http_tunnel_session(transport_type transport, const worker_handle& worker_value, std::pmr::memory_resource& resource,
        std::string_view pending = {})
        : transport_(std::move(transport)),
          resource_(resource),
          pending_(pending, &resource),
          facade_(http_tunnel_access::make(resource, worker_value, this, &read_thunk, &write_thunk, &finish_thunk, &abort_thunk)) {
        if constexpr (requires(transport_type& transport) { transport.read_datagram_input(); transport.datagram_config(); transport.send_datagram(std::span<const std::byte>{}); }) {
            http_tunnel_access::bind_datagrams(facade_, [](void* raw) -> task<std::optional<http_datagram_input>> {
                    auto& owner_value=*static_cast<http_tunnel_session*>(raw);
                    auto input=co_await owner_value.transport_.read_datagram_input();
                    if(!input){ owner_value.receive_ended_=true;
}
                    co_return input; }, [](void* raw, std::span<const std::byte> bytes_value) {
                    auto& owner_value=*static_cast<http_tunnel_session*>(raw);
                    if(owner_value.ended_){ throw std::logic_error("HTTP Datagram send direction has ended");
}
                    owner_value.transport_.send_datagram(bytes_value); }, [](void* raw) { return static_cast<http_tunnel_session*>(raw)->transport_.datagram_config(); });
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
                throw std::system_error(error, "HTTP tunnel finish");
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
                throw std::system_error(failed->error_code(), "HTTP tunnel drain");
            }
            receive_ended_ = read.end() != nullptr;
        }
    }
    [[nodiscard]] task<void> join() {
        if (http_tunnel_access::has_running_operations(facade_)) {
            abort();
        }
        co_await http_tunnel_access::close_and_join(facade_);
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
            throw std::system_error(failed->error_code(), "HTTP tunnel read");
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
            throw std::system_error(error, "HTTP tunnel write");
        }
    }
    static task<void> finish_thunk(void* raw) {
        return static_cast<http_tunnel_session*>(raw)->finish();
    }
    static void abort_thunk(void* raw) noexcept {
        static_cast<http_tunnel_session*>(raw)->abort();
    }
    transport_type transport_;
    std::pmr::memory_resource& resource_;
    std::pmr::string pending_;
    bool ended_{false};
    bool receive_ended_{false};
    bool aborted_{false};
    http_tunnel facade_;
};

template <typename transport_type>
[[nodiscard]] task<void> invoke_tunnel_handler(http_tunnel_session<transport_type>& session_value,
    connection_scanner::entry_type& entry_value, const callable_ref<void, context&>& handler, context& context_value) {
    context_tunnel_binding binding(context_value, session_value.tunnel());
    entry_value.set_phase(connection_scanner::phase_type::long_lived);
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
