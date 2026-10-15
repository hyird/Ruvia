#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

#include <asio/io_context.hpp>
#include <asio/ip/udp.hpp>
#include <asio/steady_timer.hpp>

#include "ruvia/core/task.h"

namespace ruvia::detail {

// One worker-affine DNS owner for a QUIC connection attempt. Results own their
// endpoints in the supplied worker pool, which must outlive the result. An
// operation started on this resolver must be awaited/joined before destruction;
// request_stop() runs on the owner worker and wakes DNS/timer operations.
class http3_quic_client_endpoint_resolver final {
public:
    using time_point_type = std::chrono::steady_clock::time_point;
    static constexpr std::size_t max_endpoints = 32;

    enum class status_type : std::uint8_t {
        resolved,
        timeout,
        stopped,
        resolve_failed,
        no_supported_address,
        already_resolving,
        interrupted,
    };
    struct result_type final {
        explicit result_type(std::pmr::memory_resource* resource)
            : endpoints_(resource) {}
        status_type status_{status_type::resolve_failed};
        std::error_code error_{};
        std::pmr::vector<asio::ip::udp::endpoint> endpoints_;
    };

    http3_quic_client_endpoint_resolver(asio::io_context& io,
        std::pmr::memory_resource* worker_resource);
    ~http3_quic_client_endpoint_resolver();
    http3_quic_client_endpoint_resolver(const http3_quic_client_endpoint_resolver&) = delete;
    http3_quic_client_endpoint_resolver& operator=(const http3_quic_client_endpoint_resolver&) = delete;

    // Owns hostname before returning this lazy task. The caller chooses one
    // fixed absolute connect deadline shared with QUIC handshake; do not start
    // a fresh timeout for each DNS answer or per-endpoint retry.
    [[nodiscard]] task<result_type> resolve(std::string_view host, std::uint16_t port,
        std::optional<time_point_type> absolute_deadline);
    void request_stop() noexcept;
    // Nonterminal owner-worker wake: cancel the active DNS/timer pair, drain
    // both callbacks, then allow the sole driver to resolve again using a
    // changed request deadline or admission set. Unlike request_stop(), this
    // does not prevent later resolve() calls.
    void interrupt() noexcept;

private:
    [[nodiscard]] task<result_type> resolve_owned(std::pmr::string host, std::uint16_t port,
        std::optional<time_point_type> absolute_deadline);
    void require_owner_thread() const;

    std::thread::id owner_thread_;
    std::pmr::memory_resource* resource_;
    asio::ip::udp::resolver resolver_;
    asio::steady_timer deadline_timer_;
    bool resolving_{};
    bool stopping_{};
    std::uint64_t generation_{};
};

}  // namespace ruvia::detail
