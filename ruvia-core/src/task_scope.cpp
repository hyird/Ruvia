#include "ruvia/core/task_scope.h"

#include <cstdlib>
#include <stdexcept>
#include <utility>
#include <variant>

#include "ruvia/core/memory/pmr_resource.h"

namespace ruvia {

struct task_scope::node_type {
    node_type(task_scope& owner_value, task<void> child_value)
        : scope_(owner_value),
          task_(std::move(child_value)) {}

    task_scope& scope_;
    task<void> task_;
    node_type* previous_{nullptr};
    node_type* next_{nullptr};
};

task_scope::task_scope(const worker_handle& worker_value, task_scope_options options)
    : worker_(worker_value),
      resource_(detail::pmr_resource_or_default(options.resource_)) {
    if (!worker_.valid()) {
        throw std::invalid_argument("task scope requires a valid worker");
    }
}

task_scope::~task_scope() {
    if (active_ != 0 || std::holds_alternative<task_scope_open_type>(lifecycle_) ||
        std::holds_alternative<task_scope_join_reserved_type>(lifecycle_) ||
        std::holds_alternative<task_scope_joining_type>(lifecycle_)) {
        std::terminate();
    }
}

void task_scope::spawn(task<void> task_value) & {
    if (!worker_.is_current()) {
        throw std::logic_error("task scope spawn must run on its bound worker");
    }
    if (std::holds_alternative<task_scope_join_reserved_type>(lifecycle_) ||
        std::holds_alternative<task_scope_joining_type>(lifecycle_) ||
        std::holds_alternative<task_scope_joined_type>(lifecycle_)) {
        throw std::logic_error("cannot spawn a task after task scope join started");
    }
    if (task_value.handle_ == nullptr) {
        throw std::logic_error("cannot spawn an empty ruvia::task");
    }

    std::pmr::polymorphic_allocator<node_type> allocator(resource_);
    auto* node_value = allocator.new_object<node_type>(*this, std::move(task_value));
    node_value->next_ = head_;
    if (head_ != nullptr) {
        head_->previous_ = node_value;
    }
    head_ = node_value;
    ++active_;
    lifecycle_.template emplace<task_scope_open_type>();

    node_value->task_.handle_.promise().control_.set_completion(node_value, &task_scope::child_complete);
    node_value->task_.start();
}

void task_scope::request_stop() & noexcept {
    stop_source_.request_stop();
}

stop_token task_scope::get_stop_token() const& noexcept {
    return stop_source_.token();
}

bool task_scope::stop_requested() const& noexcept {
    return stop_source_.stop_requested();
}

std::size_t task_scope::size() const& noexcept {
    return active_;
}

task<void> task_scope::join() & {
    if (!worker_.is_current()) {
        throw std::logic_error("task scope join must run on its bound worker");
    }
    if (std::holds_alternative<task_scope_join_reserved_type>(lifecycle_) ||
        std::holds_alternative<task_scope_joining_type>(lifecycle_) ||
        std::holds_alternative<task_scope_joined_type>(lifecycle_)) {
        throw std::logic_error("task scope can only be joined once");
    }
    lifecycle_.template emplace<task_scope_join_reserved_type>();
    return join_reserved(join_reservation_type(*this));
}

task<void> task_scope::join_reserved(join_reservation_type reservation) {
    auto& scope = reservation.scope();
    if (!scope.worker_.is_current()) {
        throw std::logic_error("task scope join must run on its bound worker");
    }
    co_await join_awaiter_type{scope};
}

task_scope::join_reservation_type::~join_reservation_type() {
    if (scope_ != nullptr) {
        scope_->release_join_reservation();
    }
}

void task_scope::release_join_reservation() noexcept {
    if (!std::holds_alternative<task_scope_join_reserved_type>(lifecycle_)) {
        return;
    }
    if (active_ == 0) {
        lifecycle_.template emplace<task_scope_empty_type>();
    } else {
        lifecycle_.template emplace<task_scope_open_type>();
    }
}

bool task_scope::join_awaiter_type::await_ready() const noexcept {
    return scope_.active_ == 0;
}

bool task_scope::join_awaiter_type::await_suspend(std::coroutine_handle<> continuation) {
    if (!std::holds_alternative<task_scope_join_reserved_type>(scope_.lifecycle_) ||
        std::holds_alternative<task_scope_joining_type>(scope_.lifecycle_) ||
        std::holds_alternative<task_scope_joined_type>(scope_.lifecycle_)) {
        throw std::logic_error("task scope can only be joined once");
    }
    scope_.lifecycle_.template emplace<task_scope_joining_type>(continuation);
    return true;
}

void task_scope::join_awaiter_type::await_resume() {
    if (std::holds_alternative<task_scope_join_reserved_type>(scope_.lifecycle_)) {
        scope_.lifecycle_.template emplace<task_scope_joined_type>();
    } else if (!std::holds_alternative<task_scope_joined_type>(scope_.lifecycle_)) {
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
    try {
        node_value->task_.handle_.promise().result();
    } catch (...) {
        if ((outcome_.index() == 0)) {
            outcome_ = task_scope_failure_type(std::current_exception());
            request_stop();
        }
    }

    if (node_value->previous_ != nullptr) {
        node_value->previous_->next_ = node_value->next_;
    } else {
        head_ = node_value->next_;
    }
    if (node_value->next_ != nullptr) {
        node_value->next_->previous_ = node_value->previous_;
    }
    std::pmr::polymorphic_allocator<node_type> allocator(resource_);
    allocator.delete_object(node_value);
    --active_;

    if (active_ == 0) {
        auto* joining = std::get_if<task_scope_joining_type>(&lifecycle_);
        if (joining == nullptr) {
            return;
        }
        const auto continuation = joining->continuation();
        lifecycle_.template emplace<task_scope_joined_type>();
        continuation.resume();
    }
}

void task_scope::rethrow_failure() {
    if ((outcome_.index() != 0)) {
        std::rethrow_exception(std::get<1>(outcome_).exception());
    }
}

}  // namespace ruvia
