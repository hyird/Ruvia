#include <ruvia/core/TaskScope.h>
#include <ruvia/core/memory/PmrResource.h>

#include <cstdlib>
#include <stdexcept>
#include <utility>

namespace ruvia {

struct TaskScope::Node {
    Node(TaskScope& owner, Task<void> child) : scope(owner), task(std::move(child)) {}

    TaskScope& scope;
    Task<void> task;
    Node* previous{nullptr};
    Node* next{nullptr};
};

TaskScope::TaskScope(WorkerHandle worker, std::pmr::memory_resource* resource)
    : worker_(std::move(worker)), resource_(detail::pmrResourceOrDefault(resource)) {
    if (!worker_.valid()) {
        throw std::invalid_argument("task scope requires a valid worker");
    }
}

TaskScope::~TaskScope() {
    if (active_ != 0) {
        std::terminate();
    }
}

void TaskScope::spawn(Task<void> task) {
    if (!worker_.isCurrent()) {
        throw std::logic_error("task scope spawn must run on its bound worker");
    }
    if (joinStarted_) {
        throw std::logic_error("cannot spawn a task after task scope join started");
    }

    std::pmr::polymorphic_allocator<Node> allocator(resource_);
    auto* node = allocator.new_object<Node>(*this, std::move(task));
    node->next = head_;
    if (head_ != nullptr) {
        head_->previous = node;
    }
    head_ = node;
    ++active_;

    node->task.handle_.promise().setCompletion(node, &TaskScope::childComplete);
    node->task.start();
}

void TaskScope::requestStop() noexcept {
    stopSource_.request_stop();
}

std::stop_token TaskScope::stopToken() const noexcept {
    return stopSource_.get_token();
}

bool TaskScope::stopRequested() const noexcept {
    return stopSource_.stop_requested();
}

std::size_t TaskScope::size() const noexcept {
    return active_;
}

Task<void> TaskScope::join() {
    if (!worker_.isCurrent()) {
        throw std::logic_error("task scope join must run on its bound worker");
    }
    if (joinStarted_) {
        throw std::logic_error("task scope can only be joined once");
    }
    co_await JoinAwaiter{*this};
}

bool TaskScope::JoinAwaiter::await_ready() const noexcept {
    return scope.active_ == 0;
}

bool TaskScope::JoinAwaiter::await_suspend(std::coroutine_handle<> continuation) {
    if (scope.joinStarted_) {
        throw std::logic_error("task scope can only be joined once");
    }
    scope.joinStarted_ = true;
    scope.joinContinuation_ = continuation;
    return true;
}

void TaskScope::JoinAwaiter::await_resume() {
    scope.joinStarted_ = true;
    scope.joined_ = true;
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
        if (!firstFailure_) {
            firstFailure_ = std::current_exception();
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

    if (active_ == 0 && joinContinuation_) {
        const auto continuation = std::exchange(joinContinuation_, {});
        continuation.resume();
    }
}

void TaskScope::rethrowFailure() {
    if (firstFailure_) {
        std::rethrow_exception(firstFailure_);
    }
}

}
