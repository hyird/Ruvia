#include "ruvia/core/worker_runtime_context.h"

#include <memory>
#include <utility>

#include "worker_dispatcher_impl.h"

namespace ruvia {

worker_runtime_context::worker_runtime_context(
    asio::io_context& io_context, std::size_t queue_capacity)
    : io_context_(&io_context),
      dispatcher_(std::make_shared<detail::worker_dispatcher>(io_context, queue_capacity)),
      handle_(detail::worker_handle_access::make(dispatcher_)) {}

worker_runtime_context::~worker_runtime_context() {
    detach();
}

asio::io_context& worker_runtime_context::io_context() const noexcept {
    return *io_context_;
}

const worker_handle& worker_runtime_context::handle() const noexcept {
    return handle_;
}

void worker_runtime_context::run() {
    dispatcher_->run_context({}, {}, {});
}

void worker_runtime_context::run(move_only_function<void()> startup_handler,
    move_only_function<void(std::exception_ptr)> failure_handler,
    move_only_function<void()> shutdown_handler) {
    dispatcher_->run_context(
        std::move(startup_handler), std::move(failure_handler), std::move(shutdown_handler));
}

void worker_runtime_context::close() noexcept {
    dispatcher_->close();
}

void worker_runtime_context::defer_or_terminate(move_only_function<void()> task_value) noexcept {
    dispatcher_->defer_or_terminate(std::move(task_value));
}

void worker_runtime_context::stop_timers() noexcept {
    dispatcher_->stop_timers();
}

void worker_runtime_context::detach() noexcept {
    dispatcher_->detach_context();
}

}  // namespace ruvia
