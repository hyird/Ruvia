#include "ruvia/core/task_scope.h"

#include <exception>
#include <stdexcept>
#include <utility>
#include <variant>

#include "ruvia/core/failure_report.h"
#include "ruvia/core/scoped_operation.h"
#include "ruvia/core/stop_token.h"
#include "ruvia/core/worker_ref.h"

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

task_scope::task_scope(const worker_ref& worker_value)
    : worker_(worker_value.valid()
                  ? worker_value
                  : throw std::invalid_argument("task scope requires a valid worker")) {}

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
    node_type* node_value = nullptr;
    try {
        node_value = new node_type(*this, std::move(task_value));
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
        delete node_value;
        --active_;
        resume_join_if_idle();
        throw std::logic_error("cannot spawn a task after task scope join started");
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
    detail::worker_ref_access::defer_or_terminate(
        node_value->scope_.worker_, [node_value] { node_value->scope_.finish(node_value); });
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

namespace ruvia {

namespace detail {

stop_state_ref stop_state::create() {
    return stop_state_ref(new stop_state());
}

void stop_state::destroy() noexcept {
    delete this;
}

void stop_state::request_stop() noexcept {
    std::unique_lock lock(mutex_);
    if ((flags_.load(std::memory_order_relaxed) & requested_flag) != 0) {
        return;
    }
    flags_.fetch_or(requested_flag, std::memory_order_release);
    requester_ = std::this_thread::get_id();
    while (head_ != nullptr) {
        auto* const node = head_;
        head_ = node->next_;
        if (head_ != nullptr) {
            head_->previous_ = nullptr;
        }
        node->next_ = nullptr;
        running_ = node;
        lock.unlock();
        // The callback may destroy the node, and an unlink waiting on another
        // thread frees it once running_ moves on: the node is not touched again.
        node->invoke();
        lock.lock();
        running_ = nullptr;
        if (std::exchange(run_awaited_, false)) {
            run_epoch_.fetch_add(1, std::memory_order_relaxed);
            run_epoch_.notify_all();
        }
    }
}

bool stop_state::link(stop_callback_node& node, std::atomic<stop_registration_phase>& phase) noexcept {
    std::lock_guard lock(mutex_);
    if ((flags_.load(std::memory_order_relaxed) & requested_flag) != 0) {
        return false;
    }
    node.linked_ = true;
    auto expected = stop_registration_phase::registering;
    if (!phase.compare_exchange_strong(expected, stop_registration_phase::registered,
            std::memory_order_acq_rel, std::memory_order_acquire)) {
        node.linked_ = false;
        if (expected != stop_registration_phase::reset_pending) {
            std::terminate();
        }
        return false;
    }
    node.next_ = head_;
    if (head_ != nullptr) {
        head_->previous_ = &node;
    }
    head_ = &node;
    return true;
}

void stop_state::unlink(stop_callback_node& node) noexcept {
    std::unique_lock lock(mutex_);
    if (head_ == &node || node.previous_ != nullptr) {
        if (node.previous_ != nullptr) {
            node.previous_->next_ = node.next_;
        } else {
            head_ = node.next_;
        }
        if (node.next_ != nullptr) {
            node.next_->previous_ = node.previous_;
        }
        node.previous_ = nullptr;
        node.next_ = nullptr;
        return;
    }
    // Never linked, or its run already returned.
    if (running_ != &node) {
        return;
    }
    // Destroyed from inside its own run: waiting would deadlock, and the
    // requester never touches the node after its run.
    if (requester_ == std::this_thread::get_id()) {
        return;
    }
    // The mutex orders the callback's effects before the observed running_ change.
    while (running_ == &node) {
        run_awaited_ = true;
        const auto epoch = run_epoch_.load(std::memory_order_relaxed);
        lock.unlock();
        run_epoch_.wait(epoch, std::memory_order_relaxed);
        lock.lock();
    }
}

}  // namespace detail

// Request deadlines create a source per request on every worker. The mimalloc
// resource has no shared pooling layer and outlives every observer, including
// tokens retired after the allocating worker has exited.
stop_source::stop_source()
    : state_(detail::stop_state::create()) {}

namespace {

[[nodiscard]] bool unique_parent(std::span<const stop_token> parents, std::size_t index) noexcept {
    if (!parents[index].stoppable()) {
        return false;
    }
    for (std::size_t previous = 0; previous != index; ++previous) {
        if (parents[previous] == parents[index]) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] std::size_t extra_parent_count(std::span<const stop_token> parents) noexcept {
    if (parents.size() <= 2) {
        return 0;
    }
    std::size_t count = 0;
    for (std::size_t index = 0; index != parents.size(); ++index) {
        if (parents[index].stop_requested()) {
            return 0;
        }
        count += unique_parent(parents, index);
    }
    return count > 2 ? count - 2 : 0;
}

}  // namespace

stop_group::stop_group(std::span<const stop_token> parents)
    : extra_parents_(extra_parent_count(parents)) {
    // Capture the state, never this: downstream callbacks may destroy the
    // group, including the parent registration currently invoking us.
    const auto register_parent = [this](const stop_token& parent, stop_registration& registration) {
        parent.register_callback(registration, [state = output_.state_] {
            state->request_stop();
        });
    };
    if (!extra_parents_.empty()) {
        source_.emplace();
        output_ = source_->token();
        std::size_t slot = 0;
        for (std::size_t index = 0; index != parents.size(); ++index) {
            if (!unique_parent(parents, index)) {
                continue;
            }
            if (slot < parents_.size()) {
                register_parent(parents[index], parents_[slot]);
            } else {
                register_parent(parents[index], extra_parents_[slot - parents_.size()]);
            }
            ++slot;
        }
        return;
    }
    const stop_token* first = nullptr;
    const stop_token* second = nullptr;
    for (std::size_t index = 0; index != parents.size(); ++index) {
        if (parents[index].stop_requested()) {
            output_ = parents[index];
            return;
        }
        if (unique_parent(parents, index)) {
            if (first == nullptr) {
                first = &parents[index];
            } else {
                second = &parents[index];
            }
        }
    }
    if (first == nullptr) {
        return;
    }
    if (second == nullptr) {
        output_ = *first;
        return;
    }
    source_.emplace();
    output_ = source_->token();
    register_parent(*first, parents_[0]);
    register_parent(*second, parents_[1]);
}

}  // namespace ruvia

namespace ruvia {

// Keeps a reentrant join from publishing retirement during frame/capability
// cleanup. Its registration is a value, not a frame-owner base class.
struct operation_scope::drain_guard final {
    explicit drain_guard(operation_scope& scope) noexcept {
        scope.link(registration_);
        registration_.phase_ = detail::scoped_operation_registration::phase::retiring;
    }

    ~drain_guard() {
        auto* scope = registration_.scope_;
        // Keep the drain registered while capability callbacks run: a join
        // started by one of them must not publish retirement prematurely.
        if (scope->head_ == &registration_ && registration_.next_ == nullptr &&
            scope->join_continuation_ == nullptr) {
            scope->expire_capabilities();
        }
        registration_.phase_ = detail::scoped_operation_registration::phase::expired;
        scope->unlink(registration_);
        // Join may retire this scope's owner inline. No further scope access.
        scope->resume_joiner();
    }

    detail::scoped_operation_registration registration_;
};

struct operation_scope::join_awaiter final {
    explicit join_awaiter(operation_scope& scope) noexcept
        : owner_(scope) {}

    [[nodiscard]] bool await_ready() const noexcept {
        return owner_.head_ == nullptr;
    }

    bool await_suspend(std::coroutine_handle<> continuation) noexcept {
        if (owner_.head_ == nullptr || owner_.join_continuation_ != nullptr) {
            std::terminate();
        }
        owner_.join_continuation_ = continuation;
        return true;
    }

    void await_resume() const noexcept {}

    operation_scope& owner_;
};

void operation_scope::close() noexcept {
    if (expiring_capabilities_ || (!active_ && head_ == nullptr && capability_head_ == nullptr)) {
        return;
    }
    for (auto* operation = head_; operation != nullptr; operation = operation->next_) {
        if (operation->phase_ == detail::scoped_operation_registration::phase::running ||
            operation->phase_ == detail::scoped_operation_registration::phase::completing ||
            (active_ && operation->phase_ == detail::scoped_operation_registration::phase::retiring)) {
            std::terminate();
        }
    }
    // An inactive scope already has a drain owner. Reentrant cold-frame
    // cleanup may observe its retiring entries, but never unfinished work.
    if (!active_) {
        return;
    }
    expire_for_join();
}

void operation_scope::expire() noexcept {
    if (expiring_capabilities_ || !active_) {
        return;
    }
    expire_for_join();
}

task<void> operation_scope::close_and_join() & {
    if (join_started_) {
        if (!join_complete_) {
            throw std::logic_error("scoped operation scope can only be joined once");
        }
        co_return;
    }
    join_started_ = true;
    expire_for_join();
    if (head_ == nullptr) {
        expire_capabilities();
        join_complete_ = true;
        co_return;
    }
    co_await join_awaiter(*this);
}

void operation_scope::retire_cold_operations() noexcept {
    for (;;) {
        auto* operation = head_;
        while (operation != nullptr && operation->phase_ != detail::scoped_operation_registration::phase::cold) {
            operation = operation->next_;
        }
        if (operation == nullptr) {
            return;
        }
        operation->retire_frame();
        // Cleanup may remove sibling cold frames; search the live chain again.
    }
}

void operation_scope::expire_for_join() noexcept {
    drain_guard drain(*this);
    active_ = false;
    retire_cold_operations();
}

void operation_scope::expire_capabilities() noexcept {
    expiring_capabilities_ = true;
    while (capability_head_ != nullptr) {
        capability_head_->expire();
    }
    expiring_capabilities_ = false;
}

void operation_scope::resume_joiner() noexcept {
    if (head_ != nullptr || join_continuation_ == nullptr) {
        return;
    }
    auto continuation = std::exchange(join_continuation_, {});
    expire_capabilities();
    join_complete_ = true;
    continuation.resume();
}

void operation_scope::check_worker() const noexcept {
    if (worker_ != nullptr && !worker_->is_current()) {
        std::terminate();
    }
}

scoped_capability_registration::scoped_capability_registration(
    operation_scope& scope, void* target, void (*cleanup)(void*) noexcept) noexcept
    : target_(target),
      cleanup_(cleanup),
      active_(scope.active()) {
    if (active_) {
        link(scope);
    }
}

scoped_capability_registration::scoped_capability_registration(
    const scoped_capability_registration& other, void* target) noexcept
    : target_(target),
      cleanup_(other.cleanup_),
      active_(other.active_) {
    if (active_ && other.scope_ != nullptr) {
        link(*other.scope_);
    }
}

scoped_capability_registration::scoped_capability_registration(
    scoped_capability_registration&& other, void* target) noexcept
    : scope_(other.scope_),
      previous_(other.previous_),
      next_(other.next_),
      target_(target),
      cleanup_(other.cleanup_),
      active_(other.active_) {
    if (scope_ != nullptr) {
        if (previous_ != nullptr) {
            previous_->next_ = this;
        } else {
            scope_->capability_head_ = this;
        }
        if (next_ != nullptr) {
            next_->previous_ = this;
        }
    }
    other.scope_ = nullptr;
    other.previous_ = nullptr;
    other.next_ = nullptr;
    other.target_ = nullptr;
    other.cleanup_ = nullptr;
    other.active_ = false;
}

scoped_capability_registration::~scoped_capability_registration() {
    unlink();
}

void scoped_capability_registration::require_active() const {
    if (!active_) {
        throw std::logic_error("scoped capability lifetime has expired");
    }
}

operation_scope& scoped_capability_registration::scope() const {
    require_active();
    return *scope_;
}

void scoped_capability_registration::bind(
    operation_scope& scope, void* target, void (*cleanup)(void*) noexcept) noexcept {
    if (scope_ != nullptr || active_) {
        std::terminate();
    }
    target_ = target;
    cleanup_ = cleanup;
    active_ = scope.active();
    if (active_) {
        link(scope);
    }
}

void scoped_capability_registration::link(operation_scope& scope) noexcept {
    scope_ = &scope;
    next_ = scope.capability_head_;
    if (next_ != nullptr) {
        next_->previous_ = this;
    }
    scope.capability_head_ = this;
}

void scoped_capability_registration::unlink() noexcept {
    if (scope_ == nullptr) {
        return;
    }
    if (previous_ != nullptr) {
        previous_->next_ = next_;
    } else {
        scope_->capability_head_ = next_;
    }
    if (next_ != nullptr) {
        next_->previous_ = previous_;
    }
    scope_ = nullptr;
    previous_ = nullptr;
    next_ = nullptr;
}

void scoped_capability_registration::expire() noexcept {
    const auto cleanup = cleanup_;
    auto* target = target_;
    active_ = false;
    unlink();
    // Cleanup may destroy this registration or remove sibling capabilities.
    // Never access the registration after calling its typed owner.
    if (cleanup != nullptr) {
        cleanup(target);
    }
}

void operation_scope::link(detail::scoped_operation_registration& operation) noexcept {
    operation.scope_ = this;
    operation.next_ = head_;
    if (head_ != nullptr) {
        head_->previous_ = &operation;
    }
    head_ = &operation;
}

void operation_scope::unlink(detail::scoped_operation_registration& operation) noexcept {
    if (operation.previous_ != nullptr) {
        operation.previous_->next_ = operation.next_;
    } else if (head_ == &operation) {
        head_ = operation.next_;
    }
    if (operation.next_ != nullptr) {
        operation.next_->previous_ = operation.previous_;
    }
    operation.scope_ = nullptr;
    operation.previous_ = nullptr;
    operation.next_ = nullptr;
}

namespace detail {

scoped_operation_registration::scoped_operation_registration(
    scoped_operation_registration&& other, void* new_frame_target) noexcept {
    if (other.phase_ == phase::running || other.phase_ == phase::retiring) {
        std::terminate();
    }
    if (other.phase_ == phase::unscoped) {
        return;
    }
    scope_ = other.scope_;
    previous_ = other.previous_;
    next_ = other.next_;
    phase_ = other.phase_;
    frame_target_ = other.frame_target_ != nullptr ? new_frame_target : nullptr;
    retire_cold_ = other.retire_cold_;
    if (scope_ != nullptr) {
        if (previous_ != nullptr) {
            previous_->next_ = this;
        } else {
            scope_->head_ = this;
        }
        if (next_ != nullptr) {
            next_->previous_ = this;
        }
    }
    other.scope_ = nullptr;
    other.previous_ = nullptr;
    other.next_ = nullptr;
    other.phase_ = phase::unscoped;
    other.clear_frame_binding();
}

scoped_operation_registration::~scoped_operation_registration() {
    if (scope_ != nullptr || phase_ == phase::running || phase_ == phase::retiring) {
        std::terminate();
    }
}

void scoped_operation_registration::bind(operation_scope& scope, void* target,
    void (*retire_cold)(void*) noexcept) noexcept {
    if (phase_ != phase::unscoped || retire_cold == nullptr || retire_cold_ != nullptr) {
        std::terminate();
    }
    if (!scope.active()) {
        scope.check_worker();
        phase_ = phase::retiring;
        retire_cold(target);
        phase_ = phase::expired;
        return;
    }
    phase_ = phase::cold;
    frame_target_ = target;
    retire_cold_ = retire_cold;
    scope.link(*this);
}

void scoped_operation_registration::clear_frame_binding() noexcept {
    frame_target_ = nullptr;
    retire_cold_ = nullptr;
}

void scoped_operation_registration::prepare_start() const {
    if (phase_ == phase::expired) {
        throw std::logic_error("capability operation scope has expired");
    }
    if (phase_ != phase::cold) {
        throw std::logic_error("capability operation can only be awaited once");
    }
    scope_->check_worker();
}

void scoped_operation_registration::start() noexcept {
    if (phase_ != phase::cold) {
        std::terminate();
    }
    phase_ = phase::running;
}

void scoped_operation_registration::prepare_completion() noexcept {
    if (phase_ != phase::running) {
        std::terminate();
    }
    scope_->check_worker();
    // The final suspend point permits owner relocation, not early retirement:
    // the registration stays linked until the frame has been destroyed.
    phase_ = phase::completing;
}

void scoped_operation_registration::complete() noexcept {
    if (phase_ != phase::completing) {
        std::terminate();
    }
    phase_ = phase::complete;
    clear_frame_binding();
    if (scope_ != nullptr) {
        auto* scope = scope_;
        scope->unlink(*this);
        scope->resume_joiner();
    }
}

void scoped_operation_registration::retire_frame() noexcept {
    if (phase_ == phase::running || phase_ == phase::completing || phase_ == phase::retiring) {
        std::terminate();
    }
    if (phase_ == phase::cold) {
        scope_->check_worker();
        phase_ = phase::retiring;
        const auto retire_cold = retire_cold_;
        auto* target = frame_target_;
        clear_frame_binding();
        retire_cold(target);
    }
    phase_ = phase::expired;
    if (scope_ != nullptr) {
        auto* scope = scope_;
        scope->unlink(*this);
        // Join may retire the owner inline; no further registration access.
        scope->resume_joiner();
    }
}

}  // namespace detail
}  // namespace ruvia
