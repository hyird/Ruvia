#include "ruvia/core/worker_runtime_context.h"

#include <memory>
#include <utility>

#include "ruvia/core/detail/worker/worker_dispatcher.h"

namespace ruvia {

class worker_runtime_context::impl_type final {
public:
    impl_type(asio::io_context& io_context, std::size_t queue_capacity)
        : io_context_(&io_context),
          dispatcher_(std::make_shared<detail::worker_dispatcher>(io_context, queue_capacity)),
          handle_(detail::worker_handle_access::make(dispatcher_)) {}

    ~impl_type() {
        detach();
    }

    void detach() noexcept {
        dispatcher_->detach_context();
    }

    asio::io_context* io_context_;
    std::shared_ptr<detail::worker_dispatcher> dispatcher_;
    worker_handle handle_;
};

worker_runtime_context::worker_runtime_context(
    asio::io_context& io_context, std::size_t queue_capacity)
    : impl_(std::make_unique<impl_type>(io_context, queue_capacity)) {}

worker_runtime_context::~worker_runtime_context() = default;

asio::io_context& worker_runtime_context::io_context() const noexcept {
    return *impl_->io_context_;
}

const worker_handle& worker_runtime_context::handle() const noexcept {
    return impl_->handle_;
}

worker_submission_view worker_runtime_context::submission() const& noexcept {
    return worker_submission_view(impl_->dispatcher_.get());
}

void worker_runtime_context::run() {
    impl_->dispatcher_->run_context();
}

void worker_runtime_context::run(move_only_function<void(std::exception_ptr)> failure_handler) {
    impl_->dispatcher_->run_context(std::move(failure_handler));
}

void worker_runtime_context::run(move_only_function<void()> startup_handler,
    move_only_function<void(std::exception_ptr)> failure_handler,
    move_only_function<void()> shutdown_handler) {
    impl_->dispatcher_->run_context(
        std::move(startup_handler), std::move(failure_handler), std::move(shutdown_handler));
}

void worker_runtime_context::close() noexcept {
    impl_->dispatcher_->close();
}

void worker_runtime_context::defer_or_terminate(move_only_function<void()> task_value) noexcept {
    impl_->dispatcher_->defer_or_terminate(std::move(task_value));
}

void worker_runtime_context::stop_timers() noexcept {
    impl_->dispatcher_->stop_timers();
}

void worker_runtime_context::detach() noexcept {
    impl_->dispatcher_->detach_context();
}

}  // namespace ruvia
