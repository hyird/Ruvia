#pragma once

#include <cstddef>
#include <memory>

#include <asio/io_context.hpp>

#include "ruvia/core/event_loop.h"

namespace ruvia {

struct event_loop_attachment_options final {
    std::size_t queue_capacity_{1024};
};

// Binds a worker to an io_context the caller owns. The caller may drive that
// context directly, or call event_loop_attachment::run() to install Ruvia's worker
// identity while the context is running. The attachment keeps the worker's
// endpoint valid for as long as it is alive.
// stop() and destruction are safe while another thread is inside the external
// io_context::run(): the external context service retains the worker state
// until its terminal cleanup handler runs, or until the context itself is
// destroyed. The attachment never calls io_context::stop() and does not join a
// context it does not own.
//
// After attachment retirement, event_loop handles become invalid; io_context()
// and executor() throw std::logic_error. The external io_context remains owned
// by its caller and is never stopped, restarted, or joined by the attachment.
class event_loop_attachment final {
public:
    ~event_loop_attachment();

    event_loop_attachment(const event_loop_attachment&) = delete;
    event_loop_attachment& operator=(const event_loop_attachment&) = delete;
    // Move construction transfers one attachment without touching its context.
    // Move assignment would implicitly stop the target attachment, so it stays
    // deleted and ownership transfer remains explicit.
    event_loop_attachment(event_loop_attachment&& other) noexcept;
    event_loop_attachment& operator=(event_loop_attachment&& other) = delete;

    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] event_loop loop() const noexcept;
    // Runs the attached context on the current thread with Ruvia's worker
    // identity installed. The attachment object must outlive this call.
    void run();
    void stop() noexcept;

private:
    explicit event_loop_attachment(std::shared_ptr<detail::event_loop_state> state_value) noexcept;

    std::shared_ptr<detail::event_loop_state> state_;
    friend event_loop_attachment attach_event_loop(asio::io_context&, event_loop_attachment_options);
};

// Attach a Ruvia worker to a caller-owned io_context. The caller drives the
// context with run() on exactly one thread and retains ownership of its
// unrelated work and stop/restart policy. stop() retires only Ruvia-managed
// cleanup and never cancels the caller's unrelated operations.
[[nodiscard]] event_loop_attachment attach_event_loop(
    asio::io_context& io_context, event_loop_attachment_options options = {});

}  // namespace ruvia
