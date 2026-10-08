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

#include "ruvia/core/Task.h"

namespace ruvia::detail {

// One worker-affine DNS owner for a QUIC connection attempt. Results own their
// endpoints in the supplied worker pool, which must outlive the result. An
// operation started on this resolver must be awaited/joined before destruction;
// requestStop() runs on the owner worker and wakes DNS/timer operations.
class Http3QuicClientEndpointResolver final {
public:
    using TimePoint = std::chrono::steady_clock::time_point;
    static constexpr std::size_t kMaxEndpoints = 32;

    enum class Status : std::uint8_t {
        kResolved,
        kTimeout,
        kStopped,
        kResolveFailed,
        kNoSupportedAddress,
        kAlreadyResolving,
        kInterrupted,
    };
    struct Result final {
        explicit Result(std::pmr::memory_resource* resource)
            : endpoints(resource) {}
        Status status{Status::kResolveFailed};
        std::error_code error{};
        std::pmr::vector<asio::ip::udp::endpoint> endpoints;
    };

    Http3QuicClientEndpointResolver(asio::io_context& io,
        std::pmr::memory_resource* workerResource);
    ~Http3QuicClientEndpointResolver();
    Http3QuicClientEndpointResolver(const Http3QuicClientEndpointResolver&) = delete;
    Http3QuicClientEndpointResolver& operator=(const Http3QuicClientEndpointResolver&) = delete;

    // Owns hostname before returning this lazy Task. The caller chooses one
    // fixed absolute connect deadline shared with QUIC handshake; do not start
    // a fresh timeout for each DNS answer or per-endpoint retry.
    [[nodiscard]] Task<Result> resolve(std::string_view host, std::uint16_t port,
        std::optional<TimePoint> absoluteDeadline);
    void requestStop() noexcept;
    // Nonterminal owner-worker wake: cancel the active DNS/timer pair, drain
    // both callbacks, then allow the sole driver to resolve again using a
    // changed request deadline or admission set. Unlike requestStop(), this
    // does not prevent later resolve() calls.
    void interrupt() noexcept;

private:
    [[nodiscard]] Task<Result> resolveOwned(std::pmr::string host, std::uint16_t port,
        std::optional<TimePoint> absoluteDeadline);
    void requireOwnerThread() const;

    std::thread::id ownerThread_;
    std::pmr::memory_resource* resource_;
    asio::ip::udp::resolver resolver_;
    asio::steady_timer deadlineTimer_;
    bool resolving_{};
    bool stopping_{};
    std::uint64_t generation_{};
};

}  // namespace ruvia::detail
