#include "ruvia/core/worker_handle.h"

#include <stdexcept>

#include "ruvia/core/detail/worker/worker_dispatcher.h"
#include "ruvia/core/worker_timer.h"
namespace ruvia {

worker_handle::worker_handle(std::shared_ptr<detail::worker_dispatcher> dispatcher) noexcept
    : dispatcher_(std::move(dispatcher)) {}

bool worker_handle::valid() const noexcept {
    return dispatcher_ && dispatcher_->attached();
}

bool worker_handle::accepting() const noexcept {
    return dispatcher_ && dispatcher_->accepting();
}

bool worker_handle::is_current() const noexcept {
    return dispatcher_ && dispatcher_->is_current();
}

worker_id_type worker_handle::id() const noexcept {
    return dispatcher_ ? dispatcher_->id() : 0;
}

post_result_type worker_handle::post_task(move_only_function<void()> task_value) const {
    if (!task_value) {
        throw std::invalid_argument("worker post requires a callable task");
    }
    return dispatcher_ ? dispatcher_->post(std::move(task_value))
                       : post_result_type::reject(post_status::worker_stopping, std::move(task_value));
}

worker_handle detail::worker_handle_access::make(
    const std::shared_ptr<worker_dispatcher>& dispatcher) noexcept {
    return worker_handle(dispatcher);
}

void detail::worker_handle_access::defer(const worker_handle& worker_value, move_only_function<void()> task_value) {
    const auto& dispatcher = worker_value.dispatcher_;
    if (!dispatcher) {
        throw std::runtime_error("worker stopped before internal continuation was scheduled");
    }
    dispatcher->defer(std::move(task_value));
}

bool detail::worker_handle_access::defer_if_attached(
    const worker_handle& worker_value, move_only_function<void()> task_value) {
    const auto& dispatcher = worker_value.dispatcher_;
    return dispatcher && dispatcher->defer_if_attached(std::move(task_value));
}

void detail::worker_handle_access::defer_or_terminate(
    const worker_handle& worker_value, move_only_function<void()> task_value) noexcept {
    const auto& dispatcher = worker_value.dispatcher_;
    if (!dispatcher) {
        std::terminate();
    }
    dispatcher->defer_or_terminate(std::move(task_value));
}

void detail::worker_handle_access::register_shutdown_listener(
    const worker_handle& worker_value, const std::shared_ptr<worker_shutdown_listener>& listener_value) {
    const auto& dispatcher = worker_value.dispatcher_;
    if (!dispatcher) {
        throw std::runtime_error("cannot register state on a stopped worker");
    }
    dispatcher->register_shutdown_listener(listener_value);
}

void detail::worker_handle_access::when_shutdown_notifications_complete(
    const worker_handle& worker_value, move_only_function<void()> callback_value) {
    const auto& dispatcher = worker_value.dispatcher_;
    if (!dispatcher) {
        throw std::runtime_error("cannot observe shutdown notifications on a stopped worker");
    }
    dispatcher->when_shutdown_notifications_complete(std::move(callback_value));
}

void detail::worker_handle_access::when_idle(
    const worker_handle& worker_value, move_only_function<void()> callback_value) {
    const auto& dispatcher = worker_value.dispatcher_;
    if (!dispatcher) {
        throw std::runtime_error("cannot observe idleness on a stopped worker");
    }
    dispatcher->when_idle(std::move(callback_value));
}

void detail::worker_handle_access::wait_for_reservations(const worker_handle& worker_value) noexcept {
    if (worker_value.dispatcher_) {
        worker_value.dispatcher_->wait_for_reservations();
    }
}

void worker_handle::schedule_timer(worker_timer_registration& registration,
    std::chrono::steady_clock::time_point deadline_value,
    move_only_function<void(worker_timer_outcome)> completion) const& {
    if (!dispatcher_) {
        throw std::runtime_error("cannot schedule a timer on a stopped worker");
    }
    dispatcher_->schedule_timer(registration, deadline_value, std::move(completion));
}

post_status worker_handle::post_factory(move_only_function<move_only_function<void()>()> factory) const {
    return dispatcher_ ? dispatcher_->post_factory(std::move(factory)) : post_status::worker_stopping;
}

}  // namespace ruvia
