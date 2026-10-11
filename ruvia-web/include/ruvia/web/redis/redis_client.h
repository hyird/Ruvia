#pragma once

#include <memory>

#include "ruvia/core/event_loop.h"
#include "ruvia/web/redis/redis_handle.h"

namespace ruvia {
namespace detail {
class redis_client_state;
}

// One Redis client bound to one event_loop, independent of HTTP and application.
// Handles, repositories, cold operations and PMR results borrow this owner.
// Destroy them on the bound loop before destroying the client; shutdown joins
// started operations but does not extend the allocator lifetime of results.
class redis_client final {
public:
    redis_client(event_loop loop, const redis_config& config);
    ~redis_client();
    redis_client(const redis_client&) = delete;
    redis_client& operator=(const redis_client&) = delete;
    redis_client(redis_client&&) = delete;
    redis_client& operator=(redis_client&&) = delete;

    // Lazy, single-start and worker-affine, like ordinary commands.
    [[nodiscard]] task<void> connect() &;
    task<void> connect() && = delete;
    [[nodiscard]] redis_handle with_options(operation_options options) const&;
    redis_handle with_options(operation_options) const&& = delete;
    // Returns the default-options handle. Each call registers one allocation-free
    // capability with the client operation scope, so keep the returned handle for
    // a sequence of commands instead of calling handle() per command.
    [[nodiscard]] redis_handle handle() const&;
    redis_handle handle() const&& = delete;

    // Thread-safe close request; sockets are closed only on the bound loop.
    void close() noexcept;
    // Await on the bound loop to cancel and join all started operations.
    [[nodiscard]] task<void> shutdown() &;
    task<void> shutdown() && = delete;
    [[nodiscard]] const worker_handle& worker() const& noexcept;
    const worker_handle& worker() const&& = delete;

private:
    std::shared_ptr<detail::redis_client_state> state_;
};

}  // namespace ruvia
