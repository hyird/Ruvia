#include <ruvia/core/TaskScope.h>
#include <ruvia/core/memory/PmrResource.h>

#include <cstdlib>
#include <stdexcept>
#include <utility>

namespace ruvia {

struct TaskScope::Node {
    Node(TaskScope& owner, Task<void> child)
        : scope(owner),
          task(std::move(child)) {}

    TaskScope& scope;
    Task<void> task;
    Node* previous{nullptr};
    Node* next{nullptr};
};

TaskScope::TaskScope(const WorkerHandle& worker, TaskScopeOptions options)
    : worker_(worker),
      resource_(detail::pmrResourceOrDefault(options.resource)) {
    if (!worker_.valid()) {
        throw std::invalid_argument("task scope requires a valid worker");
    }
}

TaskScope::~TaskScope() {
    if (active_ != 0 || std::holds_alternative<TaskScopeOpen>(lifecycle_) ||
        std::holds_alternative<TaskScopeJoinReserved>(lifecycle_) ||
        std::holds_alternative<TaskScopeJoining>(lifecycle_)) {
        std::terminate();
    }
}

void TaskScope::spawn(Task<void> task) & {
    if (!worker_.isCurrent()) {
        throw std::logic_error("task scope spawn must run on its bound worker");
    }
    if (std::holds_alternative<TaskScopeJoinReserved>(lifecycle_) ||
        std::holds_alternative<TaskScopeJoining>(lifecycle_) ||
        std::holds_alternative<TaskScopeJoined>(lifecycle_)) {
        throw std::logic_error("cannot spawn a task after task scope join started");
    }
    if (task.handle_ == nullptr) {
        throw std::logic_error("cannot spawn an empty ruvia::Task");
    }

    std::pmr::polymorphic_allocator<Node> allocator(resource_);
    auto* node = allocator.new_object<Node>(*this, std::move(task));
    node->next = head_;
    if (head_ != nullptr) {
        head_->previous = node;
    }
    head_ = node;
    ++active_;
    lifecycle_.template emplace<TaskScopeOpen>();

    node->task.handle_.promise().setCompletion(node, &TaskScope::childComplete);
    node->task.start();
}

void TaskScope::requestStop() & noexcept {
    stopSource_.requestStop();
}

StopToken TaskScope::stopToken() const& noexcept {
    return stopSource_.token();
}

bool TaskScope::stopRequested() const& noexcept {
    return stopSource_.stopRequested();
}

std::size_t TaskScope::size() const& noexcept {
    return active_;
}

Task<void> TaskScope::join() & {
    if (!worker_.isCurrent()) {
        throw std::logic_error("task scope join must run on its bound worker");
    }
    if (std::holds_alternative<TaskScopeJoinReserved>(lifecycle_) ||
        std::holds_alternative<TaskScopeJoining>(lifecycle_) ||
        std::holds_alternative<TaskScopeJoined>(lifecycle_)) {
        throw std::logic_error("task scope can only be joined once");
    }
    lifecycle_.template emplace<TaskScopeJoinReserved>();
    return joinReserved(JoinReservation(*this));
}

Task<void> TaskScope::joinReserved(JoinReservation reservation) {
    auto& scope = reservation.scope();
    if (!scope.worker_.isCurrent()) {
        throw std::logic_error("task scope join must run on its bound worker");
    }
    co_await JoinAwaiter{scope};
}

TaskScope::JoinReservation::~JoinReservation() {
    if (scope_ != nullptr) {
        scope_->releaseJoinReservation();
    }
}

void TaskScope::releaseJoinReservation() noexcept {
    if (!std::holds_alternative<TaskScopeJoinReserved>(lifecycle_)) {
        return;
    }
    if (active_ == 0) {
        lifecycle_.template emplace<TaskScopeEmpty>();
    } else {
        lifecycle_.template emplace<TaskScopeOpen>();
    }
}

bool TaskScope::JoinAwaiter::await_ready() const noexcept {
    return scope.active_ == 0;
}

bool TaskScope::JoinAwaiter::await_suspend(std::coroutine_handle<> continuation) {
    if (!std::holds_alternative<TaskScopeJoinReserved>(scope.lifecycle_) ||
        std::holds_alternative<TaskScopeJoining>(scope.lifecycle_) ||
        std::holds_alternative<TaskScopeJoined>(scope.lifecycle_)) {
        throw std::logic_error("task scope can only be joined once");
    }
    scope.lifecycle_.template emplace<TaskScopeJoining>(continuation);
    return true;
}

void TaskScope::JoinAwaiter::await_resume() {
    if (std::holds_alternative<TaskScopeJoinReserved>(scope.lifecycle_)) {
        scope.lifecycle_.template emplace<TaskScopeJoined>();
    } else if (!std::holds_alternative<TaskScopeJoined>(scope.lifecycle_)) {
        std::terminate();
    }
    scope.rethrowFailure();
}

void TaskScope::childComplete(void* raw) noexcept {
    auto* node = static_cast<Node*>(raw);
    try {
        detail::WorkerHandleAccess::defer(
            node->scope.worker_, [node] { node->scope.finish(node); });
    } catch (...) {
        std::terminate();
    }
}

void TaskScope::finish(Node* node) noexcept {
    try {
        node->task.handle_.promise().result();
    } catch (...) {
        if (std::holds_alternative<TaskScopeSuccess>(outcome_)) {
            outcome_.template emplace<TaskScopeFailure>(std::current_exception());
            requestStop();
        }
    }

    if (node->previous != nullptr) {
        node->previous->next = node->next;
    } else {
        head_ = node->next;
    }
    if (node->next != nullptr) {
        node->next->previous = node->previous;
    }
    std::pmr::polymorphic_allocator<Node> allocator(resource_);
    allocator.delete_object(node);
    --active_;

    if (active_ == 0) {
        auto* joining = std::get_if<TaskScopeJoining>(&lifecycle_);
        if (joining == nullptr) {
            return;
        }
        const auto continuation = joining->continuation();
        lifecycle_.template emplace<TaskScopeJoined>();
        continuation.resume();
    }
}

void TaskScope::rethrowFailure() {
    if (const auto* failure = std::get_if<TaskScopeFailure>(&outcome_)) {
        std::rethrow_exception(failure->exception());
    }
}

}  // namespace ruvia
