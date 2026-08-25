#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>
#include <ruvia/core/EventLoop.h>

namespace ruvia {

struct EventLoopPoolOptions final {
    std::size_t loopCount{0};
    std::size_t mailboxCapacity{1024};
};

class EventLoopPool final {
public:
    explicit EventLoopPool(EventLoopPoolOptions options = {});
    ~EventLoopPool();

    EventLoopPool(const EventLoopPool&) = delete;
    EventLoopPool& operator=(const EventLoopPool&) = delete;
    EventLoopPool(EventLoopPool&&) = delete;
    EventLoopPool& operator=(EventLoopPool&&) = delete;

    void start();
    void stop() noexcept;
    // Stops the pool and waits for every worker to finish. If join() happens
    // before start(), it creates short-lived owner threads to drain work accepted
    // before shutdown and to run owner-affine stop callbacks. Calling join() from
    // any worker owned by this pool throws logic_error before stopping the pool.
    void join();

    [[nodiscard]] std::size_t loopCount() const noexcept;
    [[nodiscard]] EventLoop loop(std::size_t index) const;
    [[nodiscard]] EventLoop nextLoop() noexcept;
    [[nodiscard]] EventLoop loopFor(std::uint64_t key) const noexcept;
    [[nodiscard]] EventLoop loopFor(std::string_view key) const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace ruvia
