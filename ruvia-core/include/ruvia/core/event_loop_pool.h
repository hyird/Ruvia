#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>

#include "ruvia/core/event_loop.h"

namespace ruvia {

class blocking_pool;

struct event_loop_pool_options final {
    std::size_t loop_count_{0};
    std::size_t queue_capacity_{1024};
    // One borrowed pool shared by every loop for run_blocking() and client
    // host lookups; it must outlive the pool. Null leaves the loops without one.
    blocking_pool* blocking_pool_{nullptr};
};

class event_loop_pool final {
public:
    explicit event_loop_pool(event_loop_pool_options options = {});
    ~event_loop_pool();

    event_loop_pool(const event_loop_pool&) = delete;
    event_loop_pool& operator=(const event_loop_pool&) = delete;
    event_loop_pool(event_loop_pool&&) = delete;
    event_loop_pool& operator=(event_loop_pool&&) = delete;

    void start();
    void stop() noexcept;
    // Stops the pool and waits for every worker to finish its managed cleanup.
    // If join() happens before start(), it creates short-lived owner threads to
    // drain accepted work and run stop callbacks. Loop handles become invalid
    // once joined. Calling join() from a pool worker throws logic_error before
    // stopping the pool.
    void join();

    [[nodiscard]] std::size_t loop_count() const noexcept;
    [[nodiscard]] event_loop loop(std::size_t index) const;
    [[nodiscard]] event_loop next_loop() noexcept;
    [[nodiscard]] event_loop loop_for(std::uint64_t key) const noexcept;
    [[nodiscard]] event_loop loop_for(std::string_view key) const noexcept;

private:
    struct impl_type;
    std::shared_ptr<impl_type> impl_;
};

}  // namespace ruvia
