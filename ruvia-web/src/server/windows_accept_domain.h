#pragma once

#include <cstdint>
#include <system_error>

#if defined(_WIN32)
#include <cstddef>
#include <exception>
#include <memory>
#include <memory_resource>

#include <asio/ip/tcp.hpp>

#include "ruvia/core/memory/pmr_object.h"
#include "ruvia/core/task.h"

namespace ruvia {
class worker_runtime;
}
#endif

namespace ruvia::detail {

enum class accept_error_action : std::uint8_t { retry,
    report_and_retry,
    fail };

// Peer disappearance and readiness races are normal accept outcomes. Resource
// and network failures leave the listener usable but must remain observable.
[[nodiscard]] accept_error_action classify_accept_error(const std::error_code& error) noexcept;

#if defined(_WIN32)
// A fixed origin x (listener owner x configured endpoint) accept matrix. Native
// completions only publish into their reserved slot; the socket and all Web
// callbacks stay on the origin. The domain outlives every attached worker.
class windows_accept_domain final {
public:
    struct callbacks final {
        void* target_;
        void (*accepted_)(void*, std::size_t, asio::ip::tcp::socket);
        void (*failure_)(void*, std::exception_ptr, bool) noexcept;
    };

    windows_accept_domain(std::size_t worker_count, std::size_t listener_count,
        std::pmr::memory_resource* resource);
    ~windows_accept_domain();
    windows_accept_domain(const windows_accept_domain&) = delete;
    windows_accept_domain& operator=(const windows_accept_domain&) = delete;

    // Cold setup. detach follows the worker's join and precedes destruction of
    // its core runtime (the notification borrows that runtime's context).
    void attach(std::size_t origin, worker_runtime& runtime, callbacks completion);
    void detach(std::size_t origin) noexcept;
    // Owner prepare; publish is called once, after every worker is ready.
    void stage_listener(std::size_t owner, std::size_t logical_listener, asio::ip::tcp::acceptor& listener);
    void publish();
    // Starts before prepare/ready, independently of business cancellation.
    // Returns after global native/result quiescence and its own wake/timer join.
    [[nodiscard]] task<void> run(std::size_t origin);
    void serve(std::size_t origin) noexcept;
    // Any thread: seals all issuance, including origins not yet served. Owners
    // cancel their exact OVERLAPPEDs and continue consuming until globally quiet.
    void request_stop() noexcept;

private:
    class impl_type;
    std::unique_ptr<impl_type, pmr_object_deleter<impl_type>> impl_;
};

#endif

}  // namespace ruvia::detail
