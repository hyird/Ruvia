#include "ruvia/core/event_loop_attachment.h"

#include <exception>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <utility>

#include <asio/execution_context.hpp>

#include "ruvia/core/failure_report.h"

#include "event_loop_state.h"

namespace ruvia {
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
    retirement_->close_admission();
    if (retirement_->pending() != 0) {
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

}  // namespace detail

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
    auto state_value = std::make_shared<detail::event_loop_state>(io_context, options.queue_capacity_);
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
