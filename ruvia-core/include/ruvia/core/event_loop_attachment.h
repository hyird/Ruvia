#pragma once

#include <cstddef>
#include <memory>

#include <asio/io_context.hpp>

#include "ruvia/core/event_loop.h"

namespace ruvia {

struct event_loop_attachment_options final {
    std::size_t queue_capacity_{1024};
};

// Binds a worker to an io_context the caller owns. The caller drives that
// context on exactly one thread, either directly with io_context::run() or
// with event_loop_attachment::run(), which also installs Ruvia's worker
// identity. The attachment never calls io_context::stop() and never restarts
// or joins a context it does not own.
//
// Retirement runs the same stop/finalize sequence as an event_loop_pool loop.
// stop(), or destroying the attachment, closes admission at once, from any
// thread, including while another thread is inside run(). Stop callbacks,
// root tasks, and deferred cleanup then drain, timers are cancelled, and the
// worker retires on the context's thread. Until then the attachment holds
// work on the context, so the caller keeps running it: a plain run() returns
// once retirement and the caller's own work are done. valid() turns false
// when retirement completes, for callers whose own work keeps run() busy.
//
// Destroying the io_context before retirement completes is valid only while
// no root task, stop callback, or deferred cleanup is outstanding; the
// context's shutdown then retires the worker synchronously. Destroying it
// with such work outstanding violates this contract: the shutdown reports a
// std::logic_error to the unhandled-failure diagnostic and terminates,
// because that work is owned by handlers that will never run.
//
// After retirement, event_loop handles become invalid; io_context() and
// executor() throw std::logic_error. The external io_context remains owned
// by its caller.
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

// Attach a Ruvia worker to a caller-owned io_context. The caller retains
// ownership of the context's unrelated work and stop/restart policy; stop()
// retires only Ruvia-managed work and never cancels unrelated operations.
[[nodiscard]] event_loop_attachment attach_event_loop(
    asio::io_context& io_context, event_loop_attachment_options options = {});

}  // namespace ruvia
