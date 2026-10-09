#include "ruvia/core/event_loop_pool.h"

#include <algorithm>
#include <atomic>
#include <exception>
#include <memory_resource>
#include <mutex>
#include <new>
#include <stdexcept>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include <asio/execution_context.hpp>
#include <asio/executor_work_guard.hpp>

#include "ruvia/core/detail/util/failure_report.h"
#include "ruvia/core/detail/worker/worker_dispatcher.h"
#include "ruvia/core/event_loop_attachment.h"
#include "ruvia/core/memory/process_resource.h"
#include "ruvia/core/runtime_lifecycle.h"
#include "ruvia/core/worker_runtime.h"
#include "ruvia/core/worker_runtime_context.h"

#include "worker_selection.h"

namespace ruvia {
namespace {

std::size_t default_loop_count() noexcept {
    return std::max<std::size_t>(1, std::thread::hardware_concurrency());
}

class external_context_attachment_service final : public asio::execution_context::service {
public:
    static asio::execution_context::id id;

    explicit external_context_attachment_service(asio::execution_context& context_value)
        : asio::execution_context::service(context_value) {}

    [[nodiscard]] bool claim() noexcept {
        const std::lock_guard lock(mutex_);
        if (claimed_) {
            return false;
        }
        claimed_ = true;
        return true;
    }

    void release_claim() noexcept;
    void retain(std::shared_ptr<detail::event_loop_state> state) noexcept;
    void release_state(detail::event_loop_state* state) noexcept;

private:
    void shutdown() override;

    std::mutex mutex_;
    bool claimed_{false};
    std::shared_ptr<detail::event_loop_state> state_;
};

asio::execution_context::id external_context_attachment_service::id;

class external_context_claim final {
public:
    explicit external_context_claim(asio::io_context& io_context)
        : service_(&asio::use_service<external_context_attachment_service>(io_context)) {
        if (!service_->claim()) {
            throw std::invalid_argument(
                "an io_context can have only one Ruvia event loop attachment");
        }
    }

    ~external_context_claim() {
        if (!retained_) {
            service_->release_claim();
        }
    }

    void retain(std::shared_ptr<detail::event_loop_state> state_value) noexcept {
        service_->retain(std::move(state_value));
        retained_ = true;
    }

    [[nodiscard]] external_context_attachment_service* service() const noexcept {
        return service_;
    }

    external_context_claim(const external_context_claim&) = delete;
    external_context_claim& operator=(const external_context_claim&) = delete;

private:
    external_context_attachment_service* service_;
    bool retained_{false};
};

class event_loop_retirement final {
public:
    event_loop_retirement() = default;

    void set_finalize(move_only_function<void()> finalize) noexcept {
        const std::lock_guard lock(mutex_);
        finalize_ = std::move(finalize);
    }

    [[nodiscard]] bool try_begin() noexcept {
        const std::lock_guard lock(mutex_);
        if (sealed_ || finalized_) {
            return false;
        }
        ++pending_;
        return true;
    }

    void begin() noexcept {
        if (!try_begin()) {
            std::terminate();
        }
    }

    [[nodiscard]] std::shared_ptr<void> try_acquire(bool root_admission = true) {
        {
            const std::lock_guard lock(mutex_);
            if (finalized_ || (root_admission && (root_admission_closed_ || sealed_))) {
                return {};
            }
            ++pending_;
        }
        try {
            auto token = std::allocate_shared<lease_token_type>(
                std::pmr::polymorphic_allocator<lease_token_type>(detail::process_resource()), this);
            return std::shared_ptr<void>(std::move(token), this);
        } catch (...) {
            finish();
            throw;
        }
    }

    void finish() noexcept {
        move_only_function<void()> finalize;
        {
            const std::lock_guard lock(mutex_);
            if (pending_ == 0) {
                std::terminate();
            }
            --pending_;
            if (sealed_ && pending_ == 0 && !finalized_) {
                finalized_ = true;
                finalize = std::move(finalize_);
            }
        }
        if (finalize) {
            finalize();
        }
    }

    [[nodiscard]] std::size_t pending() const noexcept {
        const std::lock_guard lock(mutex_);
        return pending_;
    }

    void close_root_admission() noexcept {
        const std::lock_guard lock(mutex_);
        root_admission_closed_ = true;
    }

    void close_admission() noexcept {
        const std::lock_guard lock(mutex_);
        root_admission_closed_ = true;
        sealed_ = true;
    }

    void seal() noexcept {
        move_only_function<void()> finalize;
        {
            const std::lock_guard lock(mutex_);
            sealed_ = true;
            if (pending_ == 0 && !finalized_) {
                finalized_ = true;
                finalize = std::move(finalize_);
            }
        }
        if (finalize) {
            finalize();
        }
    }

private:
    struct lease_token_type final {
        explicit lease_token_type(event_loop_retirement* owner_value) noexcept
            : owner_(owner_value) {}
        ~lease_token_type() {
            owner_->finish();
        }
        event_loop_retirement* owner_;
    };

    mutable std::mutex mutex_;
    std::size_t pending_{0};
    bool root_admission_closed_{false};
    bool sealed_{false};
    bool finalized_{false};
    move_only_function<void()> finalize_;
};

// The callback owner is kept alive by the completion handler until both the
// returned task frame and its completion chain have retired.
class event_loop_stop_listener final : public detail::worker_shutdown_listener,
                                       public std::enable_shared_from_this<event_loop_stop_listener> {
public:
    event_loop_stop_listener(worker_handle worker_value, asio::io_context::executor_type executor,
        move_only_function<task<void>()> callback_value, detail::event_loop_failure_sink_type failure_sink,
        std::shared_ptr<event_loop_retirement> retirement)
        : worker_(std::move(worker_value)),
          executor_(std::move(executor)),
          callback_(std::move(callback_value)),
          failure_sink_(std::move(failure_sink)),
          retirement_(std::move(retirement)) {}

    void worker_stopping() noexcept override {
        if (!callback_) {
            return;
        }
        retirement_->begin();
        auto self = shared_from_this();
        if (worker_.is_current()) {
            start_callback(std::move(self));
            return;
        }
        try {
            detail::worker_handle_access::defer(worker_,
                [self = std::move(self)]() mutable noexcept { self->start_callback(std::move(self)); });
        } catch (...) {
            report(failure_sink_, std::current_exception());
            std::terminate();
        }
    }

private:
    static void report(
        const detail::event_loop_failure_sink_type& sink_value, std::exception_ptr failure) noexcept {
        if (sink_value) {
            sink_value(std::move(failure));
            return;
        }
        // An attached loop has no pool to hold the failure for join().
        detail::report_unhandled_failure("event loop stop callback", failure);
    }

    void start_callback(std::shared_ptr<event_loop_stop_listener> self) noexcept {
        std::optional<task<void>> task;
        try {
            task.emplace(callback_());
        } catch (const std::bad_alloc&) {
            report(failure_sink_, std::current_exception());
            std::terminate();
        } catch (...) {
            report(failure_sink_, std::current_exception());
            retirement_->finish();
            return;
        }

        try {
            detail::async_start_task(std::move(*task), asio::bind_executor(executor_,
                                                           [self = std::move(self)](detail::task_completion_result<void> result_value) mutable {
                                                               if (const auto* failure = result_value.failure()) {
                                                                   report(self->failure_sink_, failure->exception());
                                                               }
                                                               self->retirement_->finish();
                                                           }));
        } catch (...) {
            report(failure_sink_, std::current_exception());
            std::terminate();
        }
    }

    worker_handle worker_;
    asio::io_context::executor_type executor_;
    move_only_function<task<void>()> callback_;
    detail::event_loop_failure_sink_type failure_sink_;
    std::shared_ptr<event_loop_retirement> retirement_;
};

}  // namespace

namespace detail {

struct event_loop_state final : worker_shutdown_listener,
                                std::enable_shared_from_this<event_loop_state> {
    using context_ownership_type = std::variant<worker_runtime, external_context_claim>;

    explicit event_loop_state(std::size_t queue_capacity)
        : context_ownership_(std::in_place_type<worker_runtime>,
              worker_runtime_options{.queue_capacity_ = queue_capacity}),
          io_context_(&owned_runtime().context().io_context()),
          executor_(io_context_->get_executor()),
          runtime_(owned_runtime().context()) {}

    event_loop_state(asio::io_context& external_context, std::size_t queue_capacity)
        : context_ownership_(std::in_place_type<external_context_claim>, external_context),
          io_context_(std::addressof(external_context)),
          executor_(external_context.get_executor()),
          work_(asio::make_work_guard(*io_context_)),
          attached_runtime_(std::in_place, *io_context_, queue_capacity),
          runtime_(*attached_runtime_) {}

    [[nodiscard]] worker_runtime& owned_runtime() noexcept {
        return *std::get_if<worker_runtime>(&context_ownership_);
    }

    void retain_external_context(const std::shared_ptr<event_loop_state>& self) noexcept {
        if (auto* claim = std::get_if<external_context_claim>(&context_ownership_)) {
            claim->retain(self);
        }
    }

    void install_lifecycle_listener() {
        worker_handle_access::register_shutdown_listener(runtime_.handle(), shared_from_this());
    }

    void worker_stopping() noexcept override {}

    void worker_stopping_complete() noexcept override {
        stop(true, shared_from_this());
    }

    void report_failure(std::exception_ptr failure) noexcept {
        if (!failure) {
            return;
        }
        if (failure_sink_) {
            failure_sink_(std::move(failure));
            return;
        }
        report_unhandled_failure("attached event loop failure", failure);
        stop(true, shared_from_this());
    }

    void report_attached_failure(std::exception_ptr failure) noexcept {
        if (!failure) {
            return;
        }
        report_unhandled_failure("attached event loop failure", failure);
        stop(true, shared_from_this());
    }

    [[nodiscard]] std::shared_ptr<void> acquire_root_lease() {
        return retirement_->try_acquire();
    }

    [[noreturn]] void fail_teardown_start(std::exception_ptr failure) noexcept {
        if (failure_sink_) {
            failure_sink_(std::move(failure));
        } else {
            report_unhandled_failure("event loop teardown startup", std::move(failure));
        }
        std::terminate();
    }

    void stop(bool runtime_started, std::shared_ptr<event_loop_state> keep_alive) noexcept {
        if (stopping_.exchange(true, std::memory_order_acq_rel)) {
            return;
        }
        retirement_->close_root_admission();
        if (!retirement_->try_begin()) {
            return;
        }
        std::weak_ptr<event_loop_state> weak = keep_alive;
        retirement_->set_finalize([weak] {
            auto state_value = weak.lock();
            if (!state_value) {
                return;
            }
            try {
                detail::worker_handle_access::when_idle(state_value->runtime_.handle(), [weak] {
                    auto idle_state = weak.lock();
                    if (!idle_state) {
                        return;
                    }
                    auto failure_owner = idle_state;
                    const auto worker_value = idle_state->runtime_.handle();
                    auto finalization_keep_alive = std::move(idle_state);
                    try {
                        detail::worker_handle_access::defer(worker_value,
                            [state_value = std::move(finalization_keep_alive)] { state_value->finalize_stop(); });
                    } catch (...) {
                        failure_owner->fail_teardown_start(std::current_exception());
                    }
                });
            } catch (...) {
                state_value->fail_teardown_start(std::current_exception());
            }
        });
        if (std::holds_alternative<external_context_claim>(context_ownership_)) {
            runtime_.close();
            if (!runtime_started || runtime_.handle().is_current()) {
                runtime_.stop_timers();
            } else {
                try {
                    detail::worker_handle_access::defer(runtime_.handle(),
                        [keep_alive = std::move(keep_alive)] { keep_alive->runtime_.stop_timers(); });
                } catch (...) {
                    fail_teardown_start(std::current_exception());
                }
            }
        } else {
            owned_runtime().request_stop();
        }
        // close() can lose the race to a dispatcher that already set
        // accepting=false but has not yet called every shutdown listener. Do
        // not seal the retirement gate until that first notification batch is
        // fully complete, so every listener can acquire its cleanup lease.
        try {
            detail::worker_handle_access::when_shutdown_notifications_complete(runtime_.handle(),
                [retirement = retirement_] {
                    retirement->seal();
                    retirement->finish();
                });
        } catch (...) {
            fail_teardown_start(std::current_exception());
        }
    }

    void finalize_stop() noexcept {
        if (std::holds_alternative<external_context_claim>(context_ownership_)) {
            work_->reset();
            runtime_.detach();
            std::get<external_context_claim>(context_ownership_).service()->release_state(this);
        } else {
            owned_runtime().finalize();
        }
    }

    void shutdown_external_context() noexcept {
        if (!std::holds_alternative<external_context_claim>(context_ownership_)) {
            return;
        }
        retirement_->close_admission();
        if (retirement_->pending() != 0) {
            std::terminate();
        }
        stopping_.store(true, std::memory_order_release);
        runtime_.stop_timers();
        work_->reset();
        runtime_.detach();
        detail::worker_handle_access::wait_for_reservations(runtime_.handle());
    }

    context_ownership_type context_ownership_;
    asio::io_context* const io_context_;
    asio::io_context::executor_type executor_;
    std::optional<asio::executor_work_guard<asio::io_context::executor_type>> work_;
    std::optional<worker_runtime_context> attached_runtime_;
    worker_runtime_context& runtime_;
    std::atomic_bool stopping_{false};
    // Bound once before publication to the stable pool owner or weak attachment
    // owner. Failure reporting and stop registration only read this channel.
    event_loop_failure_sink_type failure_sink_;
    std::shared_ptr<event_loop_retirement> retirement_{std::make_shared<event_loop_retirement>()};
};

}  // namespace detail

namespace {

void external_context_attachment_service::release_claim() noexcept {
    const std::lock_guard lock(mutex_);
    claimed_ = false;
}

void external_context_attachment_service::retain(
    std::shared_ptr<detail::event_loop_state> state_value) noexcept {
    const std::lock_guard lock(mutex_);
    state_ = std::move(state_value);
}

void external_context_attachment_service::release_state(detail::event_loop_state* state_value) noexcept {
    std::shared_ptr<detail::event_loop_state> abandoned;
    {
        const std::lock_guard lock(mutex_);
        if (state_.get() != state_value) {
            return;
        }
        abandoned = std::move(state_);
        claimed_ = false;
    }
    abandoned.reset();
}

void external_context_attachment_service::shutdown() {
    std::shared_ptr<detail::event_loop_state> state;
    {
        const std::lock_guard lock(mutex_);
        state = std::move(state_);
        claimed_ = false;
    }
    if (state) {
        // The context is entering Asio's service shutdown, so no new handler
        // may be queued here. Retire the worker before the context destroys
        // its scheduler. The state may still be held by event_loop/Attachment;
        // those handles become terminal and no longer expose a dangling
        // io_context reference.
        state->shutdown_external_context();
    }
}

}  // namespace

class event_loop_pool_owner final : public std::enable_shared_from_this<event_loop_pool_owner> {
public:
    struct failure_record_type final {
        std::mutex mutex_;
        std::exception_ptr first_;
        bool observed_{true};

        void record(std::exception_ptr failure) noexcept {
            if (!failure) {
                return;
            }
            {
                const std::lock_guard lock(mutex_);
                if (observed_) {
                    if (!first_) {
                        first_ = std::move(failure);
                    }
                    return;
                }
            }
            detail::report_unhandled_failure("event loop failure after pool retirement", failure);
        }

        void detach_observer() noexcept {
            std::exception_ptr remaining;
            {
                const std::lock_guard lock(mutex_);
                observed_ = false;
                remaining = std::exchange(first_, nullptr);
            }
            if (remaining) {
                detail::report_unhandled_failure("event loop failure during pool retirement", remaining);
            }
        }

        [[nodiscard]] std::exception_ptr take() noexcept {
            const std::lock_guard lock(mutex_);
            return std::exchange(first_, nullptr);
        }
    };

    explicit event_loop_pool_owner(std::size_t loop_count) {
        loops_.reserve(loop_count);
    }

    void add_loop(const std::shared_ptr<detail::event_loop_state>& loop) {
        loops_.emplace_back(loop);
    }

    void record_failure(std::exception_ptr failure) noexcept {
        if (!failure) {
            return;
        }
        failure_record_.record(std::move(failure));
    }

    void report_failure(std::exception_ptr failure) noexcept {
        if (!failure) {
            return;
        }
        record_failure(failure);
        stop();
    }

    void stop() noexcept {
        const bool runtime_started = lifecycle_.state() != runtime_lifecycle::state_type::ready;
        if (!lifecycle_.request_stop()) {
            return;
        }
        for (const auto& weak_loop : loops_) {
            if (auto loop = weak_loop.lock()) {
                loop->stop(runtime_started, std::move(loop));
            }
        }
    }

    [[nodiscard]] bool start() noexcept {
        return lifecycle_.start();
    }

    void complete_stop() noexcept {
        lifecycle_.complete_stop();
    }

    [[nodiscard]] std::exception_ptr take_failure() noexcept {
        return failure_record_.take();
    }

    void detach_pool_observer() noexcept {
        failure_record_.detach_observer();
    }

private:
    std::pmr::vector<std::weak_ptr<detail::event_loop_state>> loops_{detail::process_resource()};
    runtime_lifecycle lifecycle_;
    failure_record_type failure_record_;
};

event_loop_stop_registration::event_loop_stop_registration(
    std::shared_ptr<detail::worker_shutdown_listener> listener_value) noexcept
    : listener_(std::move(listener_value)) {}

bool event_loop_stop_registration::valid() const noexcept {
    return listener_ != nullptr;
}

void event_loop_stop_registration::reset() noexcept {
    listener_.reset();
}

event_loop::event_loop(std::shared_ptr<detail::event_loop_state> state_value) noexcept
    : state_(std::move(state_value)) {}

bool event_loop::valid() const noexcept {
    return state_ && state_->runtime_.handle().valid();
}

bool event_loop::accepting() const noexcept {
    return state_ && state_->runtime_.handle().accepting();
}

bool event_loop::is_current() const noexcept {
    return state_ && state_->runtime_.handle().is_current();
}

worker_id_type event_loop::id() const noexcept {
    return state_ ? state_->runtime_.handle().id() : 0;
}

asio::io_context& event_loop::io_context() const& {
    if (!state_ || !state_->runtime_.handle().valid()) {
        if (state_) {
            throw std::logic_error("event loop execution context is detached");
        }
        throw std::logic_error("cannot access a default-constructed event loop");
    }
    return *state_->io_context_;
}

asio::io_context::executor_type event_loop::executor() const {
    static_cast<void>(io_context());
    return state_->executor_;
}

const worker_handle& event_loop::dispatch_handle() const noexcept {
    if (state_) {
        return state_->runtime_.handle();
    }
    static const worker_handle empty_handle;
    return empty_handle;
}

worker_handle event_loop::handle() const noexcept {
    return dispatch_handle();
}

void event_loop::report_failure(std::exception_ptr failure) const noexcept {
    if (!failure) {
        return;
    }
    if (state_) {
        state_->report_failure(std::move(failure));
    } else {
        detail::report_unhandled_failure("invalid event loop failure", std::move(failure));
    }
}

detail::event_loop_failure_sink_type event_loop::failure_sink() const {
    if (!state_) {
        throw std::logic_error("cannot start a task on an invalid event loop");
    }
    return state_->failure_sink_;
}

std::shared_ptr<void> event_loop::acquire_root_lease() const {
    if (!state_) {
        return {};
    }
    return state_->acquire_root_lease();
}

bool event_loop::defer_cleanup_task(move_only_function<void()> task_value) const {
    if (!task_value) {
        throw std::invalid_argument("event loop cleanup must be callable");
    }
    if (!state_) {
        return false;
    }
    auto lease_value = state_->retirement_->try_acquire(false);
    if (!lease_value) {
        return false;
    }
    struct cleanup_control final {
        // Destroy inputs and the retirement lease before releasing the context
        // owner. Do not rely on lambda capture member destruction order.
        std::shared_ptr<detail::event_loop_state> state_;
        std::shared_ptr<void> lease_;
        move_only_function<void()> task_;
    };
    return detail::worker_handle_access::defer_if_attached(state_->runtime_.handle(),
        [control = cleanup_control{state_, std::move(lease_value), std::move(task_value)}]() mutable { control.task_(); });
}

event_loop_stop_registration event_loop::register_stop_callback(
    move_only_function<task<void>()> callback_value) const {
    if (!state_) {
        throw std::logic_error("cannot register a stop callback on an invalid event loop");
    }
    if (!callback_value) {
        throw std::invalid_argument("event loop stop callback must be callable");
    }
    if (!state_->runtime_.handle().accepting()) {
        throw std::runtime_error("cannot register a stop callback on a stopping event loop");
    }
    auto listener_value = std::make_shared<event_loop_stop_listener>(state_->runtime_.handle(),
        state_->executor_, std::move(callback_value), state_->failure_sink_,
        state_->retirement_);
    detail::worker_handle_access::register_shutdown_listener(state_->runtime_.handle(), listener_value);
    return event_loop_stop_registration(std::move(listener_value));
}

event_loop_attachment::event_loop_attachment(std::shared_ptr<detail::event_loop_state> state_value) noexcept
    : state_(std::move(state_value)) {}

event_loop_attachment::~event_loop_attachment() {
    stop();
}

event_loop_attachment::event_loop_attachment(event_loop_attachment&& other) noexcept
    : state_(std::move(other.state_)) {}

bool event_loop_attachment::valid() const noexcept {
    return state_ != nullptr && state_->runtime_.handle().valid();
}

event_loop event_loop_attachment::loop() const noexcept {
    return event_loop(state_);
}

void event_loop_attachment::run() {
    auto state_value = state_;
    if (!state_value) {
        throw std::logic_error("cannot run an invalid event loop attachment");
    }
    if (!state_value->runtime_.handle().valid()) {
        throw std::logic_error("event loop execution context is detached");
    }
    state_value->runtime_.run();
}

void event_loop_attachment::stop() noexcept {
    if (state_) {
        state_->stop(true, state_);
    }
}

event_loop_attachment attach_event_loop(
    asio::io_context& io_context, event_loop_attachment_options options) {
    auto state_value = std::make_shared<detail::event_loop_state>(io_context, options.queue_capacity_);
    const std::weak_ptr<detail::event_loop_state> weak_state = state_value;
    // root_task uses this sink directly; it must not call event_loop_state::report_failure,
    // which would route back through this same sink.
    state_value->failure_sink_ = [weak_state](std::exception_ptr failure) {
        if (const auto locked_state = weak_state.lock()) {
            locked_state->report_attached_failure(std::move(failure));
        } else {
            detail::report_unhandled_failure(
                "attached event loop failure after retirement", std::move(failure));
        }
    };
    state_value->install_lifecycle_listener();
    state_value->retain_external_context(state_value);
    return event_loop_attachment(std::move(state_value));
}

struct event_loop_pool::impl_type {
    explicit impl_type(event_loop_pool_options options) {
        const auto count = options.loop_count_ == 0 ? default_loop_count() : options.loop_count_;
        if (options.queue_capacity_ == 0) {
            throw std::invalid_argument("event loop queue capacity must be greater than zero");
        }
        owner_ = std::make_shared<event_loop_pool_owner>(count);
        loops_.reserve(count);
        for (std::size_t i = 0; i < count; ++i) {
            loops_.push_back(std::make_shared<detail::event_loop_state>(options.queue_capacity_));
            loops_.back()->install_lifecycle_listener();
            owner_->add_loop(loops_.back());
            loops_.back()->failure_sink_ = [stable_owner = owner_](std::exception_ptr failure) {
                stable_owner->report_failure(std::move(failure));
            };
            const std::weak_ptr<detail::event_loop_state> weak = loops_.back();
            auto* const stable_runtime = &loops_.back()->owned_runtime();
            stable_runtime->configure({
                .stop_admission_ = [weak, stable_runtime] {
                    if (auto loop = weak.lock()) {
                        loop->stop(true, std::move(loop));
                    } else {
                        // Construction rollback can destroy an unpublished
                        // state before its core owner drains this control.
                        stable_runtime->finalize();
                    } },
                .failure_ = [stable_owner = owner_](std::exception_ptr failure) noexcept { stable_owner->report_failure(std::move(failure)); },
            });
        }
    }

    void stop() noexcept {
        owner_->stop();
    }

    void join_loops() {
        for (const auto& loop : loops_) {
            loop->owned_runtime().join();
        }
    }

    std::pmr::vector<std::shared_ptr<detail::event_loop_state>> loops_{detail::process_resource()};
    std::shared_ptr<event_loop_pool_owner> owner_;
    std::atomic<std::size_t> next_index_{0};
};

event_loop_pool::event_loop_pool(event_loop_pool_options options)
    : impl_(std::make_unique<impl_type>(options)) {}

event_loop_pool::~event_loop_pool() {
    stop();
    try {
        join();
    } catch (...) {
        // Destroying a pool that was never joined explicitly makes this the
        // only place its first failure is ever rethrown, and a destructor
        // cannot rethrow it further. Report rather than end here.
        detail::report_unhandled_failure("event loop pool", std::current_exception());
    }
    impl_->owner_->detach_pool_observer();
}

void event_loop_pool::start() {
    if (!impl_->owner_->start()) {
        throw std::logic_error("event loop pool can only be started once");
    }
    try {
        for (const auto& loop : impl_->loops_) {
            loop->owned_runtime().start();
        }
    } catch (...) {
        const auto launch_failure = std::current_exception();
        impl_->stop();
        impl_->join_loops();
        impl_->owner_->complete_stop();
        std::rethrow_exception(launch_failure);
    }
}

void event_loop_pool::stop() noexcept {
    impl_->stop();
}

void event_loop_pool::join() {
    if (std::ranges::any_of(
            impl_->loops_, [](const auto& loop) { return loop->runtime_.handle().is_current(); })) {
        throw std::logic_error("cannot join an event loop pool from one of its workers");
    }
    impl_->stop();
    impl_->join_loops();
    for (const auto& loop : impl_->loops_) {
        detail::worker_handle_access::wait_for_reservations(loop->runtime_.handle());
    }
    impl_->owner_->complete_stop();

    if (const auto failure = impl_->owner_->take_failure()) {
        std::rethrow_exception(failure);
    }
}

std::size_t event_loop_pool::loop_count() const noexcept {
    return impl_->loops_.size();
}

event_loop event_loop_pool::loop(std::size_t index) const {
    if (index >= impl_->loops_.size()) {
        throw std::out_of_range("event loop index is out of range");
    }
    return event_loop(impl_->loops_[index]);
}

event_loop event_loop_pool::next_loop() noexcept {
    const auto index = impl_->next_index_.fetch_add(1, std::memory_order_relaxed);
    return event_loop(impl_->loops_[index % impl_->loops_.size()]);
}

event_loop event_loop_pool::loop_for(std::uint64_t key) const noexcept {
    return event_loop(impl_->loops_[key % impl_->loops_.size()]);
}

event_loop event_loop_pool::loop_for(std::string_view key) const noexcept {
    return loop_for(detail::worker_selection_hash(key));
}

}  // namespace ruvia
