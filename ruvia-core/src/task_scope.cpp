#include "ruvia/core/task_scope.h"

#include <exception>
#include <stdexcept>
#include <utility>
#include <variant>

#include "ruvia/core/failure_report.h"
#include "ruvia/core/memory/pmr_object.h"

namespace ruvia {

struct task_scope::node_type {
    node_type(task_scope& owner_value, task<void> child_value)
        : scope_(owner_value),
          task_(std::move(child_value)) {}

    task_scope& scope_;
    task<void> task_;
};

struct task_scope::retiring_guard final {
    explicit retiring_guard(task_scope& scope) noexcept
        : scope_(&scope),
          previous_(scope.retiring_) {
        scope.retiring_ = this;
    }

    ~retiring_guard() {
        if (!destroyed_) {
            scope_->retiring_ = previous_;
        }
    }

    task_scope* scope_;
    retiring_guard* previous_;
    bool destroyed_{false};
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
    for (auto* retiring = retiring_; retiring != nullptr; retiring = retiring->previous_) {
        retiring->destroyed_ = true;
    }
    // Every child finished but no join observed the outcome: the failure has
    // no other owner.
    if (std::holds_alternative<task_scope_open_type>(lifecycle_) && failure_ != nullptr) {
        report_unhandled_failure("task scope child", failure_);
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
    task_value.prepare_direct_start();

    // Admission must remain visible while the allocator can reenter join.
    ++active_;
    std::pmr::polymorphic_allocator<node_type> allocator(resource_);
    node_type* node_value = nullptr;
    try {
        node_value = allocator.new_object<node_type>(*this, std::move(task_value));
    } catch (...) {
        // Destroy the still-cold child before allowing a join to retire its owner.
        {
            task<void> discarded(std::move(task_value));
        }
        --active_;
        resume_join_if_idle();
        throw;
    }
    if (join_started()) {
        allocator.delete_object(node_value);
        --active_;
        resume_join_if_idle();
        throw std::logic_error("cannot spawn a task after task scope join started");
    }

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

task_scope::join_operation task_scope::join() & {
    if (!worker_.is_current()) {
        throw std::logic_error("task scope join must run on its bound worker");
    }
    return join_operation(*this);
}

// Only a cold operation may move; a started one has handed its continuation
// to the scope.
task_scope::join_operation::join_operation(join_operation&& other) noexcept
    : scope_(std::exchange(other.scope_, nullptr)) {
    if (other.started_) {
        std::terminate();
    }
}

bool task_scope::join_operation::await_ready() {
    if (scope_ == nullptr || started_) {
        throw std::logic_error("task scope join cannot be awaited");
    }
    if (!scope_->worker_.is_current()) {
        throw std::logic_error("task scope join must run on its bound worker");
    }
    if (std::holds_alternative<task_scope_joining_type>(scope_->lifecycle_) ||
        std::holds_alternative<task_scope_joined_type>(scope_->lifecycle_)) {
        throw std::logic_error("task scope can only be joined once");
    }
    started_ = true;
    if (scope_->active_ == 0 && scope_->retiring_ == nullptr) {
        scope_->lifecycle_.template emplace<task_scope_joined_type>();
        return true;
    }
    return false;
}

void task_scope::join_operation::await_suspend(std::coroutine_handle<> continuation) noexcept {
    scope_->lifecycle_.template emplace<task_scope_joining_type>(continuation);
}

void task_scope::join_operation::await_resume() {
    if (!std::holds_alternative<task_scope_joined_type>(scope_->lifecycle_)) {
        std::terminate();
    }
    scope_->rethrow_failure();
}

void task_scope::resume_join_if_idle() noexcept {
    if (active_ != 0 || retiring_ != nullptr) {
        return;
    }
    auto* joining = std::get_if<task_scope_joining_type>(&lifecycle_);
    if (joining == nullptr) {
        return;
    }
    auto continuation = joining->continuation();
    lifecycle_.template emplace<task_scope_joined_type>();
    // The joiner may destroy this scope inline.
    continuation.resume();
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
    node_value->task_.prepare_direct_completion();
    try {
        node_value->task_.handle_.promise().result();
    } catch (...) {
        if (failure_ == nullptr) {
            failure_ = std::current_exception();
            stop_requested_ = true;
            notify_stop = true;
        }
    }
    if (notify_stop) {
        request_stop();
    }

    --active_;
    {
        retiring_guard retiring(*this);
        std::pmr::polymorphic_allocator<node_type> allocator(resource_);
        node_value->task_.finish_direct_completion();
        allocator.delete_object(node_value);
        if (retiring.destroyed_) {
            return;
        }
    }
    resume_join_if_idle();
}

void task_scope::rethrow_failure() {
    if (failure_ != nullptr) {
        std::rethrow_exception(failure_);
    }
}

}  // namespace ruvia
