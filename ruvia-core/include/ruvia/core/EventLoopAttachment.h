#pragma once

#include <cstddef>
#include <memory>

#include <asio/io_context.hpp>

#include <ruvia/core/EventLoop.h>

namespace ruvia {

struct EventLoopAttachmentOptions final {
    std::size_t mailboxCapacity{1024};
};

// Binds a worker to an io_context the caller owns and drives with run(). The
// attachment keeps the worker's endpoint valid for as long as it is alive.
// stop() and destruction are safe while another thread is inside run(): the
// external context service retains the worker state until its terminal cleanup
// handler runs, or until the context itself is destroyed. The attachment never
// calls io_context::stop() and does not join a context it does not own.
//
// If the external io_context is destroyed first, attached EventLoop handles
// become terminal; ioContext() and executor() then throw std::logic_error.
class EventLoopAttachment final {
public:
    ~EventLoopAttachment();

    EventLoopAttachment(const EventLoopAttachment&) = delete;
    EventLoopAttachment& operator=(const EventLoopAttachment&) = delete;
    // Move construction transfers one attachment without touching its context.
    // Move assignment would implicitly stop the target attachment, so it stays
    // deleted and ownership transfer remains explicit.
    EventLoopAttachment(EventLoopAttachment&& other) noexcept;
    EventLoopAttachment& operator=(EventLoopAttachment&& other) = delete;

    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] EventLoop loop() const noexcept;
    void stop() noexcept;

private:
    explicit EventLoopAttachment(std::shared_ptr<detail::EventLoopState> state) noexcept;

    std::shared_ptr<detail::EventLoopState> state_;
    friend EventLoopAttachment attachEventLoop(asio::io_context&, EventLoopAttachmentOptions);
};

// Attach a Ruvia worker to a caller-owned io_context. The caller drives the
// context with run() on exactly one thread and retains ownership of its
// unrelated work and stop/restart policy.
[[nodiscard]] EventLoopAttachment attachEventLoop(asio::io_context& ioContext, EventLoopAttachmentOptions options = {});

}  // namespace ruvia
