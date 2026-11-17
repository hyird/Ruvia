#include "task_scope.h"

#include <coroutine>
#include <cstddef>
#include <exception>
#include <memory>
#include <stdexcept>
#include <utility>
#include <variant>

#include "failure_report.h"
#include "stop_token.h"
#include "task.h"
#include "worker_ref.h"

namespace ruvia {

struct task_scope::node_type {
    node_type(task_scope& owner_value, task<void>&& child_value) :
        scope_(owner_value),
        task_(std::move(child_value)) {}

    task_scope& scope_;
    task<void> task_;
};

struct task_scope::retiring_guard final {
    explicit retiring_guard(task_scope& scope) noexcept :
        scope_(&scope),
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

task_scope::task_scope(const worker_ref& worker_value) :
    worker_(worker_value.valid() ? worker_value : throw std::invalid_argument("task scope requires a valid worker")) {}

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
        return std::holds_alternative<task_scope_joining_type>(lifecycle_) || std::holds_alternative<task_scope_joined_type>(lifecycle_);
    };
    if (join_started()) {
        throw std::logic_error("cannot spawn a task after task scope join started");
    }
    task_value.prepare_direct_start();

    // Admission must remain visible while the allocator can reenter join.
    ++active_;
    node_type* node_value = nullptr;
    try {
        auto pending = std::make_unique<node_type>(*this, std::move(task_value));
        if (join_started()) {
            throw std::logic_error("cannot spawn a task after task scope join started");
        }
        // Node allocation can reenter and retire a scoped cold frame.
        pending->task_.prepare_direct_start();
        node_value = pending.release();
    } catch (...) {
        // If allocation never transferred the child, retire it before releasing admission.
        {
            task<void> discarded(std::move(task_value));
        }
        --active_;
        resume_join_if_idle();
        throw;
    }

    node_value->task_.coroutine_.promise().control_.set_completion(node_value, &task_scope::child_complete);
    node_value->task_.start();
}

void task_scope::request_stop() & noexcept {
    stop_source_.request_stop();
}

stop_token task_scope::token() const& {
    return stop_source_.token();
}

bool task_scope::stop_requested() const& noexcept {
    return stop_source_.stop_requested();
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
task_scope::join_operation::join_operation(join_operation&& other) noexcept :
    scope_(std::exchange(other.scope_, nullptr)) {
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
    if (std::holds_alternative<task_scope_joining_type>(scope_->lifecycle_) || std::holds_alternative<task_scope_joined_type>(scope_->lifecycle_)) {
        throw std::logic_error("task scope can only be joined once");
    }
    started_ = true;
    if (scope_->active_ == 0 && scope_->retiring_ == nullptr) {
        scope_->lifecycle_.template emplace<task_scope_joining_type>();
        return true;
    }
    return false;
}

void task_scope::join_operation::await_suspend(std::coroutine_handle<> continuation) noexcept {
    scope_->lifecycle_.template emplace<task_scope_joining_type>(task_scope_joining_type{continuation});
}

void task_scope::join_operation::await_resume() {
    const auto* joining = std::get_if<task_scope_joining_type>(&scope_->lifecycle_);
    if (joining == nullptr || joining->continuation_) {
        std::terminate();
    }
    scope_->lifecycle_.template emplace<task_scope_joined_type>();
    scope_->rethrow_failure();
}

void task_scope::resume_join_if_idle() noexcept {
    if (active_ != 0 || retiring_ != nullptr) {
        return;
    }
    auto* joining = std::get_if<task_scope_joining_type>(&lifecycle_);
    if (joining != nullptr && joining->continuation_) {
        // Keep the join pending until await_resume(), even after queuing it.
        const auto continuation = std::exchange(joining->continuation_, {});
        const auto resume = [continuation] {
            continuation.resume();
        };
        if (!worker_.defer_completion(resume)) {
            // An external loop can retire its Worker before running an already
            // queued child completion. Complete the join on that final turn.
            continuation.resume();
        }
    }
}

void task_scope::child_complete(void* raw) noexcept {
    auto* node_value = static_cast<node_type*>(raw);
    detail::worker_ref_access::defer_or_terminate(node_value->scope_.worker_, [node_value] {
        node_value->scope_.finish(node_value);
    });
}

void task_scope::finish(node_type* node_value) noexcept {
    bool notify_stop = false;
    node_value->task_.prepare_direct_completion();
    try {
        node_value->task_.coroutine_.promise().result();
    } catch (...) {
        if (failure_ == nullptr) {
            failure_ = std::current_exception();
            notify_stop = true;
        } else if (!equivalent_failures(std::current_exception(), failure_)) {
            // join rethrows only the first failure; later distinct ones have no other owner.
            report_unhandled_failure("additional task scope child", std::current_exception());
        }
    }
    if (notify_stop) {
        request_stop();
    }

    --active_;
    {
        retiring_guard retiring(*this);
        task<void> retired_task(std::move(node_value->task_));
        delete node_value;
        retired_task.finish_direct_completion();
        retired_task.reset();
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
