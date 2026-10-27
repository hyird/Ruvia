#include "ruvia/core/event_loop.h"

#include <exception>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <stdexcept>
#include <utility>
#include <variant>

#include <asio/execution_context.hpp>

#include "ruvia/core/failure_report.h"

#include "event_loop_state.h"

namespace ruvia {
namespace {

// Reports through the loop's failure sink while the loop is alive, or to the
// unhandled-failure diagnostic under `context` once it has retired.
void report_through_loop(const std::weak_ptr<detail::event_loop_state>& loop, std::exception_ptr failure,
    const char* context) noexcept {
    if (const auto state_value = loop.lock(); state_value && state_value->failure_sink_) {
        state_value->failure_sink_(std::move(failure));
        return;
    }
    report_unhandled_failure(context, std::move(failure));
}

}  // namespace

namespace detail {

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

    void release_claim() noexcept {
        const std::lock_guard lock(mutex_);
        claimed_ = false;
    }

    void retain(std::shared_ptr<event_loop_state> state_value) noexcept {
        const std::lock_guard lock(mutex_);
        state_ = std::move(state_value);
    }

    void release_state(event_loop_state* state_value) noexcept {
        std::shared_ptr<event_loop_state> abandoned;
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

private:
    void shutdown() override {
        std::shared_ptr<event_loop_state> state;
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

    std::mutex mutex_;
    bool claimed_{false};
    std::shared_ptr<event_loop_state> state_;
};

asio::execution_context::id external_context_attachment_service::id;

external_context_claim::external_context_claim(asio::io_context& io_context)
    : service_(&asio::use_service<external_context_attachment_service>(io_context)) {
    if (!service_->claim()) {
        throw std::invalid_argument("an io_context can have only one Ruvia event loop attachment");
    }
}

external_context_claim::~external_context_claim() {
    if (!retained_) {
        service_->release_claim();
    }
}

void external_context_claim::retain(std::shared_ptr<event_loop_state> state_value) noexcept {
    service_->retain(std::move(state_value));
    retained_ = true;
}

void external_context_claim::release(event_loop_state* state_value) noexcept {
    service_->release_state(state_value);
}

attached_context::attached_context(asio::io_context& io_context, std::size_t queue_capacity)
    : claim_(io_context),
      runtime_(io_context, queue_capacity),
      stop_(runtime_) {}

// The callback owner is kept alive by the completion handler until both the
// returned task frame and its completion chain have retired.
class event_loop_stop_listener final : public std::enable_shared_from_this<event_loop_stop_listener> {
public:
    event_loop_stop_listener(worker_handle worker_value, asio::io_context::executor_type executor,
        move_only_function<task<void>()> callback_value, std::weak_ptr<event_loop_state> loop)
        : worker_(std::move(worker_value)),
          executor_(std::move(executor)),
          callback_(std::move(callback_value)),
          loop_(std::move(loop)) {}

    void install_shutdown_hook() {
        worker_handle_access::register_shutdown_hook(worker_, shutdown_hook_, shared_from_this(),
            worker_shutdown_operations<event_loop_stop_listener>);
    }

    void cancel() noexcept {
        move_only_function<task<void>()> abandoned;
        std::shared_ptr<event_loop_state> active_loop;
        {
            const std::lock_guard lock(mutex_);
            if (started_ || cancelled_) {
                return;
            }
            cancelled_ = true;
            if (std::exchange(retirement_begun_, false)) {
                active_loop = std::move(active_loop_);
            }
        }
        // Cancellation owns the callback after publishing cancelled_; neither
        // the stop notification nor a deferred start may use it now.
        abandoned = std::move(callback_);
        abandoned = nullptr;
        if (active_loop) {
            active_loop->retirement_.finish();
        }
    }

    void worker_stopping() noexcept {
        auto loop = loop_.lock();
        if (!loop) {
            return;
        }
        {
            const std::lock_guard lock(mutex_);
            if (cancelled_ || !callback_ || retirement_begun_ || started_) {
                return;
            }
            loop->retirement_.begin();
            active_loop_ = std::move(loop);
            retirement_begun_ = true;
        }
        auto self = shared_from_this();
        if (worker_.is_current()) {
            start_callback(std::move(self));
            return;
        }
        try {
            worker_handle_access::defer(worker_,
                [self = std::move(self)]() mutable noexcept { self->start_callback(std::move(self)); });
        } catch (...) {
            report(std::current_exception());
            std::terminate();
        }
    }

private:
    void report(std::exception_ptr failure) const noexcept {
        report_through_loop(loop_, std::move(failure), "event loop stop callback");
    }

    void start_callback(std::shared_ptr<event_loop_stop_listener> self) noexcept {
        {
            const std::lock_guard lock(mutex_);
            if (cancelled_ || !callback_ || !retirement_begun_) {
                return;
            }
            started_ = true;
        }
        std::optional<task<void>> task;
        try {
            task.emplace(callback_());
        } catch (const std::bad_alloc&) {
            report(std::current_exception());
            std::terminate();
        } catch (...) {
            report(std::current_exception());
            auto active_loop = std::move(active_loop_);
            active_loop->retirement_.finish();
            return;
        }

        try {
            async_start_task(std::move(*task),
                asio::bind_executor(executor_,
                    [self = std::move(self)](std::exception_ptr failure) mutable {
                        if (failure != nullptr) {
                            self->report(std::move(failure));
                        }
                        auto active_loop = std::move(self->active_loop_);
                        active_loop->retirement_.finish();
                    }));
        } catch (...) {
            report(std::current_exception());
            std::terminate();
        }
    }

    // First member: unlinks from the worker registry after every other member is gone.
    worker_shutdown_hook shutdown_hook_;
    worker_handle worker_;
    asio::io_context::executor_type executor_;
    std::mutex mutex_;
    bool retirement_begun_{false};
    bool cancelled_{false};
    bool started_{false};
    move_only_function<task<void>()> callback_;
    std::weak_ptr<event_loop_state> loop_;
    std::shared_ptr<event_loop_state> active_loop_;
};

event_loop_state::event_loop_state(std::size_t queue_capacity)
    : context_ownership_(std::in_place_type<worker_runtime>,
          worker_runtime_options{.queue_capacity_ = queue_capacity}),
      io_context_(&owned_runtime().io_context()),
      executor_(io_context_->get_executor()),
      runtime_(owned_runtime().context()) {}

event_loop_state::event_loop_state(asio::io_context& external_context, std::size_t queue_capacity)
    : context_ownership_(std::in_place_type<attached_context>, external_context, queue_capacity),
      io_context_(std::addressof(external_context)),
      executor_(external_context.get_executor()),
      runtime_(std::get_if<attached_context>(&context_ownership_)->runtime_) {}

void event_loop_state::attach(const std::shared_ptr<event_loop_state>& self) {
    auto& attached = *std::get_if<attached_context>(&context_ownership_);
    attached.stop_.configure({
        // Only stop() requests this chain, so its gate is already sealing.
        // Joining it keeps the chain from finalizing before the gate drains.
        .stop_admission_ = [this] { stop(); },
        // Releasing the claim can destroy this state together with its chain;
        // the chain then releases its work guard on the still-running context.
        .finalize_ =
            [this, &attached] {
                runtime_.detach();
                attached.claim_.release(this);
            },
    });
    attached.claim_.retain(self);
}

void event_loop_state::shutdown_external_context() noexcept {
    // Only attach() retains a state with this service, so it is an attached loop.
    auto& attached = *std::get_if<attached_context>(&context_ownership_);
    retirement_.close_admission();
    if (retirement_.pending() != 0) {
        // Their frames and leases are owned by handlers that this shutdown
        // destroys without running; nothing can retire them safely.
        report_unhandled_failure("attached event loop",
            std::make_exception_ptr(std::logic_error(
                "io_context destroyed while its attached event loop still had root tasks, stop "
                "callbacks, or cleanup pending; keep running the context until the attachment retires")));
        std::terminate();
    }
    stopping_.store(true, std::memory_order_release);
    attached.stop_.abandon();
    runtime_.detach();
    worker_handle_access::wait_for_reservations(runtime_.handle());
}
void event_loop_state::install_shutdown_hook() {
    worker_handle_access::register_shutdown_hook(
        runtime_.handle(), shutdown_hook_, shared_from_this(), worker_shutdown_operations<event_loop_state>);
}

void event_loop_state::worker_stopping_complete() noexcept {
    stop();
}

void event_loop_state::report_failure(std::exception_ptr failure) noexcept {
    if (failure_sink_) {
        failure_sink_(std::move(failure));
        return;
    }
    report_attached_failure(std::move(failure));
}

void event_loop_state::report_attached_failure(std::exception_ptr failure) noexcept {
    report_unhandled_failure("attached event loop failure", failure);
    stop();
}

void event_loop_state::fail_teardown_start(std::exception_ptr failure) noexcept {
    if (failure_sink_) {
        failure_sink_(std::move(failure));
    } else {
        report_unhandled_failure("event loop teardown startup", std::move(failure));
    }
    std::terminate();
}

void event_loop_state::stop() noexcept {
    if (stopping_.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    retirement_.close_root_admission();
    if (!retirement_.try_begin()) {
        return;
    }
    const std::weak_ptr<event_loop_state> weak = weak_from_this();
    retirement_.set_finalize([weak] {
        if (auto state_value = weak.lock()) {
            auto* const state_ptr = state_value.get();
            state_ptr->defer_finalize_stop(std::move(state_value));
        }
    });
    if (auto* attached = std::get_if<attached_context>(&context_ownership_)) {
        attached->stop_.request_stop();
    } else {
        owned_runtime().request_stop();
    }
    // close() can lose the race to a dispatcher that already set
    // accepting=false but has not yet called every shutdown listener. Do
    // not seal the retirement gate until both that first notification batch
    // and the worker's queued posts have drained, so every listener and
    // draining post can acquire its cleanup lease.
    try {
        worker_handle_access::when_shutdown_notifications_complete(runtime_.handle(), [weak] {
            if (const auto state_value = weak.lock()) {
                state_value->retirement_.finish();
                state_value->schedule_finalize_when_idle();
            }
        });
    } catch (...) {
        fail_teardown_start(std::current_exception());
    }
}

void event_loop_state::schedule_finalize_when_idle() noexcept {
    const std::weak_ptr<event_loop_state> weak = weak_from_this();
    try {
        worker_handle_access::when_idle(runtime_.handle(), [weak] {
            if (const auto idle_state = weak.lock()) {
                idle_state->retirement_.seal();
            }
        });
    } catch (...) {
        fail_teardown_start(std::current_exception());
    }
}

void event_loop_state::defer_finalize_stop(std::shared_ptr<event_loop_state> self) noexcept {
    const auto worker_value = runtime_.handle();
    try {
        // A detached context ran shutdown_external_context(), which
        // already abandoned the stop chain this would finalize.
        (void)worker_handle_access::defer_if_attached(
            worker_value, [state_value = std::move(self)] { state_value->finalize_stop(); });
    } catch (...) {
        fail_teardown_start(std::current_exception());
    }
}

void event_loop_state::finalize_stop() noexcept {
    if (auto* attached = std::get_if<attached_context>(&context_ownership_)) {
        attached->stop_.finalize();
    } else {
        owned_runtime().finalize();
    }
}

void report_root_task_failure(const std::weak_ptr<event_loop_state>& loop, std::exception_ptr failure) noexcept {
    report_through_loop(loop, std::move(failure), "unobserved event-loop root task");
}

}  // namespace detail

event_loop_stop_registration::event_loop_stop_registration(
    std::shared_ptr<detail::event_loop_stop_listener> listener_value) noexcept
    : listener_(std::move(listener_value)) {}

bool event_loop_stop_registration::valid() const noexcept {
    return listener_ != nullptr;
}

void event_loop_stop_registration::reset() noexcept {
    if (auto listener = std::move(listener_)) {
        listener->cancel();
    }
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
        report_unhandled_failure("invalid event loop failure", std::move(failure));
    }
}

std::shared_ptr<void> event_loop::acquire_root_lease() const {
    if (!state_) {
        return {};
    }
    return state_->retirement_.try_acquire(state_);
}

bool event_loop::defer_cleanup_task(move_only_function<void()> task_value) const {
    if (!task_value) {
        throw std::invalid_argument("event loop cleanup must be callable");
    }
    if (!state_) {
        return false;
    }
    auto lease_value = state_->retirement_.try_acquire(state_, false);
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
    auto listener_value = std::allocate_shared<detail::event_loop_stop_listener>(
        std::pmr::polymorphic_allocator<detail::event_loop_stop_listener>(process_resource()),
        state_->runtime_.handle(), state_->executor_, std::move(callback_value), state_);
    listener_value->install_shutdown_hook();
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
        state_->stop();
    }
}

event_loop_attachment attach_event_loop(
    asio::io_context& io_context, event_loop_attachment_options options) {
    auto state_value = std::allocate_shared<detail::event_loop_state>(
        std::pmr::polymorphic_allocator<detail::event_loop_state>(process_resource()),
        io_context, options.queue_capacity_);
    const std::weak_ptr<detail::event_loop_state> weak_state = state_value;
    // root_task uses this sink directly; it must not call event_loop_state::report_failure,
    // which would route back through this same sink.
    state_value->failure_sink_ = [weak_state](std::exception_ptr failure) {
        if (const auto locked_state = weak_state.lock()) {
            locked_state->report_attached_failure(std::move(failure));
        } else {
            report_unhandled_failure("attached event loop failure after retirement", std::move(failure));
        }
    };
    state_value->install_shutdown_hook();
    state_value->attach(state_value);
    return event_loop_attachment(std::move(state_value));
}

}  // namespace ruvia
