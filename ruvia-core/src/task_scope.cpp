#include "ruvia/core/task_scope.h"

#include <exception>
#include <stdexcept>
#include <utility>
#include <variant>

#include "ruvia/core/failure_report.h"
#include "ruvia/core/memory/pmr_resource.h"

namespace ruvia {

struct task_scope::node_type {
    node_type(task_scope& owner_value, task<void> child_value)
        : scope_(owner_value),
          task_(std::move(child_value)) {}

    task_scope& scope_;
    task<void> task_;
};

task_scope::task_scope(const worker_handle& worker_value, task_scope_options options)
    : worker_(worker_value),
      resource_(pmr_resource_or_default(options.resource_)) {
    if (!worker_.valid()) {
        throw std::invalid_argument("task scope requires a valid worker");
    }
}

task_scope::~task_scope() {
    if (active_ != 0 || std::holds_alternative<task_scope_joining_type>(lifecycle_)) {
        std::terminate();
    }
    // Every child finished but no join observed the outcome: the failure has
    // no other owner.
    if (std::holds_alternative<task_scope_open_type>(lifecycle_) && outcome_.index() != 0) {
        report_unhandled_failure("task scope child", std::get<1>(outcome_).exception());
    }
}

void task_scope::spawn(task<void> task_value) & {
    if (!worker_.is_current()) {
        throw std::logic_error("task scope spawn must run on its bound worker");
    }
    const auto join_started = [this] {
        return std::holds_alternative<task_scope_joining_type>(lifecycle_) ||
               std::holds_alternative<task_scope_joined_type>(lifecycle_);
    };
    if (join_started()) {
        throw std::logic_error("cannot spawn a task after task scope join started");
    }
    if (task_value.handle_ == nullptr) {
        throw std::logic_error("cannot spawn an empty ruvia::task");
    }

    std::pmr::polymorphic_allocator<node_type> allocator(resource_);
    auto* node_value = allocator.new_object<node_type>(*this, std::move(task_value));
    // The scope's resource may reenter it and start a join while allocating;
    // that join owns the scope, so the unstarted child is discarded.
    if (join_started()) {
        allocator.delete_object(node_value);
        throw std::logic_error("cannot spawn a task after task scope join started");
    }
    ++active_;

    node_value->task_.handle_.promise().control_.set_completion(node_value, &task_scope::child_complete);
    node_value->task_.start();
}

void task_scope::request_stop() & noexcept {
    stop_requested_ = true;
    if (stop_source_) {
        stop_source_->request_stop();
    }
}

stop_token task_scope::get_stop_token() const& {
    if (!stop_source_) {
        stop_source_.emplace();
        if (stop_requested_) {
            stop_source_->request_stop();
        }
    }
    return stop_source_->token();
}

bool task_scope::stop_requested() const& noexcept {
    return stop_requested_;
}

std::size_t task_scope::size() const& noexcept {
    return active_;
}

task<void> task_scope::join() & {
    if (!worker_.is_current()) {
        throw std::logic_error("task scope join must run on its bound worker");
    }
    return join_scope(*this);
}

task<void> task_scope::join_scope(task_scope& scope) {
    co_await join_awaiter_type{scope};
}

bool task_scope::join_awaiter_type::await_ready() const {
    if (!scope_.worker_.is_current()) {
        throw std::logic_error("task scope join must run on its bound worker");
    }
    if (std::holds_alternative<task_scope_joining_type>(scope_.lifecycle_) ||
        std::holds_alternative<task_scope_joined_type>(scope_.lifecycle_)) {
        throw std::logic_error("task scope can only be joined once");
    }
    if (scope_.active_ == 0) {
        scope_.lifecycle_.template emplace<task_scope_joined_type>();
        return true;
    }
    return false;
}

void task_scope::join_awaiter_type::await_suspend(std::coroutine_handle<> continuation) noexcept {
    scope_.lifecycle_.template emplace<task_scope_joining_type>(continuation);
}

void task_scope::join_awaiter_type::await_resume() {
    if (!std::holds_alternative<task_scope_joined_type>(scope_.lifecycle_)) {
        std::terminate();
    }
    scope_.rethrow_failure();
}

void task_scope::child_complete(void* raw) noexcept {
    auto* node_value = static_cast<node_type*>(raw);
    try {
        detail::worker_handle_access::defer(
            node_value->scope_.worker_, [node_value] { node_value->scope_.finish(node_value); });
    } catch (...) {
        std::terminate();
    }
}

void task_scope::finish(node_type* node_value) noexcept {
    bool notify_stop = false;
    try {
        node_value->task_.handle_.promise().result();
    } catch (...) {
        if (outcome_.index() == 0) {
            outcome_ = task_scope_failure_type(std::current_exception());
            stop_requested_ = true;
            notify_stop = true;
        }
    }

    const bool last_active = --active_ == 0;
    std::coroutine_handle<> join_continuation{};
    if (last_active) {
        if (auto* joining = std::get_if<task_scope_joining_type>(&lifecycle_)) {
            join_continuation = joining->continuation();
            lifecycle_.template emplace<task_scope_joined_type>();
        }
    }

    if (notify_stop) {
        request_stop();
    }

    std::pmr::polymorphic_allocator<node_type> allocator(resource_);
    allocator.delete_object(node_value);

    if (join_continuation) {
        join_continuation.resume();
    }
}

void task_scope::rethrow_failure() {
    if (outcome_.index() != 0) {
        std::rethrow_exception(std::get<1>(outcome_).exception());
    }
}

}  // namespace ruvia
