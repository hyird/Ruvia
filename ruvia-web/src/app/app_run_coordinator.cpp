#include "app/app_run_coordinator.h"

#include <algorithm>
#include <chrono>
#include <csignal>
#include <exception>
#include <memory>
#include <memory_resource>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

#include <asio/signal_set.hpp>

#include "ruvia/core/failure_report.h"
#include "ruvia/core/worker_runtime.h"
#include "ruvia/core/worker_timer.h"
#include "ruvia/web/app.h"
#include "ruvia/web/detail/controller/controller_runtime.h"

#include "app/app_config_guards.h"
#include "app/app_runtime_graph.h"
#include "app/app_state.h"
#include "http/static_root_index.h"
#include "router/router_impl.h"
#include "server/http_server_options_validation.h"

namespace ruvia {
namespace {

void add_shutdown_signals(asio::signal_set& signals) {
    signals.add(SIGINT);
    signals.add(SIGTERM);
#if defined(SIGBREAK)
    signals.add(SIGBREAK);
#endif
}

void acceptor_failed(void* object) noexcept {
    static_cast<application*>(object)->stop();
}

bool acceptor_target_available(void* object) noexcept {
    return static_cast<detail::web_worker_runtime*>(object)->available_for_network_dispatch();
}

void acceptor_target_accept(void* object, detail::native_accepted_socket_ticket&& ticket) noexcept {
    static_cast<detail::web_worker_runtime*>(object)->accept_transferred_connection(std::move(ticket));
}

void invoke_stop_hooks(detail::app_state& state_value) noexcept {
    for (auto& hook : state_value.on_stop_hooks_) {
        try {
            hook();
        } catch (...) {
            ruvia::report_unhandled_failure("app stop hook", std::current_exception());
        }
    }
}

// Process shutdown signals for one run, on their own core runtime. The first
// signal restores the default disposition and requests the application's
// stop, so a repeated signal is not swallowed while the stop drains -- or
// while a slow startup hook still runs, before the stop can take effect.
class process_signal_handling final {
public:
    explicit process_signal_handling(application& owner)
        : runtime_({.queue_capacity_ = 128}),
          signals_(runtime_.context().io_context()) {
        runtime_.configure({
            .stop_admission_ = [this] {
                std::error_code ignored;
                signals_.clear(ignored);
                signals_.cancel(ignored);
                runtime_.finalize(); },
            .failure_ = [&owner](std::exception_ptr) noexcept { owner.stop(); },
        });
        // A failed registration or thread start must join the runtime while
        // signals_ is alive: its stop hook still clears and cancels the set,
        // and member unwinding would destroy signals_ before runtime_.
        try {
            add_shutdown_signals(signals_);
            signals_.async_wait([this, &owner](const std::error_code& ec, int) {
                if (!ec) {
                    std::error_code ignored;
                    signals_.clear(ignored);
                    owner.stop();
                }
            });
            // Nothing else can reach this freshly built runtime, so no stop
            // can precede its launch.
            (void)runtime_.start();
        } catch (...) {
            stop();
            throw;
        }
    }

    ~process_signal_handling() {
        stop();
    }

    process_signal_handling(const process_signal_handling&) = delete;
    process_signal_handling& operator=(const process_signal_handling&) = delete;

    void stop() noexcept {
        runtime_.request_stop();
        runtime_.join();
    }

    [[nodiscard]] std::exception_ptr failure() const noexcept {
        return runtime_.failure();
    }

private:
    worker_runtime runtime_;
    asio::signal_set signals_;
};

class app_run_coordinator final {
public:
    app_run_coordinator(application& owner_value, detail::app_state& state_value)
        : owner_(owner_value),
          state_(state_value),
          runtime_resource_(process_resource()) {}

    void run() {
        begin_run();

        try {
            build_and_publish_runtime();
        } catch (...) {
            // Rethrown to the run() caller, who owns reporting it.
            complete_unpublished_run();
            throw;
        }

        auto& group = state_.runtime_->group_;
        std::exception_ptr primary_failure;
        bool served = false;
        try {
            if (!stop_requested()) {
                start_signal_handling();
                // Every worker and the acceptor are ready before start hooks;
                // no listener accepts until every hook has succeeded.
                if (group.start()) {
                    publish_ready_workers();
                    run_start_hooks();
                    if (!stop_requested() && group.serve()) {
                        wait_for_stop();
                        served = true;
                    }
                }
            }
        } catch (...) {
            primary_failure = std::current_exception();
            owner_.stop();
        }

        stop_signal_handling();
        if (served) {
            drain();
        }
        // Stop admission is distinct from final retirement: stop hooks run
        // after admission closes and before any worker is finalized.
        group.request_stop();
        invoke_stop_hooks(state_);
        // The rethrown failure is chosen before the group reports the member
        // failures it supersedes, so the library reports no failure twice and
        // never the rethrown one, whose reporting belongs to the run() caller.
        const auto signal_failure = signals_.has_value() ? signals_->failure() : nullptr;
        const auto rethrown = group.join(primary_failure != nullptr ? primary_failure : signal_failure);
        retire_runtime();
        if (signal_failure != nullptr && !ruvia::equivalent_failures(signal_failure, rethrown)) {
            ruvia::report_unhandled_failure("app signal handling", signal_failure);
        }
        if (rethrown != nullptr) {
            std::rethrow_exception(rethrown);
        }
    }

private:
    void begin_run() {
        std::lock_guard lock(state_.mutex_);
        detail::ensure_app_not_running(state_.lifecycle_.active(), "app is already running");
        if (state_.listeners_.empty()) {
            throw std::invalid_argument(
                "application::run() requires at least one listener; call application::listen() first");
        }
        if (!state_.lifecycle_.begin_run()) {
            std::terminate();
        }
    }

    void build_and_publish_runtime() {
        const auto controller_registrars = detail::seal_controller_registrars();
        auto runtime =
            make_pmr_object<detail::app_runtime_graph>(runtime_resource_, runtime_resource_);
        auto prepared_options = state_.options_;
        prepared_options.env_ = &state_.env_;
        prepared_options.failure_ = detail::worker_failure_sink{
            .target_ = &owner_,
            .invoke_ =
                [](void* target, const std::exception_ptr&) noexcept {
                    static_cast<application*>(target)->stop();
                },
        };

        if (state_.document_root_config_.has_value()) {
            const auto& document_root = *state_.document_root_config_;
            // Precompressed variants are negotiated by the compression policy,
            // so without it the refresher never builds them.
            const auto precompression = prepared_options.compression_.has_value()
                                            ? document_root.precompression_
                                            : detail::static_root_precompression_options{};
            runtime->document_root_ = make_pmr_object<detail::document_root_publisher>(runtime_resource_,
                http_make_path_from_native_path(document_root.root_), document_root.static_options_,
                document_root.runtime_.refresh_interval_, precompression, document_root.resident_);
            prepared_options.document_root_ =
                detail::http_server_options::document_root_type::refreshing(*runtime->document_root_);
        }
        if (state_.blocking_pool_.has_value()) {
            runtime->blocking_pool_ =
                make_pmr_object<blocking_pool>(runtime_resource_, *state_.blocking_pool_);
            prepared_options.blocking_pool_ = runtime->blocking_pool_.get();
        }

        const auto validated_configuration =
            detail::validate_http_server_configuration(state_.listeners_, std::move(prepared_options));

        // One immutable route plan per run, declared against the first worker's
        // controller instances; every worker only binds its own instances.
        detail::controller_store first_controllers(controller_registrars);
        runtime->route_plan_ = detail::compile_route_plan(
            runtime_resource_, controller_registrars, first_controllers, state_.configuration_);
        runtime->workers_.reserve(state_.options_.server_.worker_count_);
        for (std::size_t i = 0; i < state_.options_.server_.worker_count_; ++i) {
            auto controllers =
                i == 0 ? std::move(first_controllers) : detail::controller_store(controller_registrars);
            auto routes_value = make_pmr_object<detail::route_table>(
                runtime_resource_, runtime_resource_, *runtime->route_plan_, controllers);
            const detail::worker_capability_definitions capabilities{
#ifdef RUVIA_ENABLE_DATABASE
                .databases_ = std::span<const detail::db_definition>(state_.databases_),
#endif
#ifdef RUVIA_ENABLE_REDIS
                .redis_ = std::span<const detail::redis_definition_type>(state_.redis_),
#endif
                .worker_states_ = state_.configuration_.worker_states(),
                .http_clients_ = std::span<const detail::http_client_definition_type>(state_.http_clients_),
            };
            auto worker_value = make_pmr_object<detail::web_worker_runtime>(
                runtime_resource_, validated_configuration, *routes_value, capabilities);
            runtime->workers_.emplace_back(
                std::move(controllers), std::move(routes_value), std::move(worker_value));
        }

        std::pmr::vector<detail::acceptor::worker_target> acceptor_targets(runtime_resource_);
        acceptor_targets.reserve(runtime->workers_.size());
        for (auto& slot : runtime->workers_) {
            auto* target = slot.runtime_.get();
            acceptor_targets.push_back({
                .submission_ = target->network_submission(),
                .object_ = target,
                .available_ = &acceptor_target_available,
                .accept_ = &acceptor_target_accept,
            });
        }
        runtime->ingress_ = make_pmr_object<detail::acceptor>(runtime_resource_,
            validated_configuration.listeners(), acceptor_targets, &owner_, &acceptor_failed);
        runtime->ingress_->prepare();
        // Ingress is the external producer of every worker.
        runtime->group_.add(runtime->ingress_->runtime(), 0);
        for (auto& slot : runtime->workers_) {
            runtime->group_.add(slot.runtime_->runtime(), 1);
        }

        std::lock_guard lock(state_.mutex_);
        state_.runtime_ = std::move(runtime);
        if (!state_.lifecycle_.publish_runtime() && !state_.lifecycle_.stop_requested()) {
            std::terminate();
        }
    }

    void complete_unpublished_run() noexcept {
        std::lock_guard lock(state_.mutex_);
        if (state_.runtime_ != nullptr) {
            std::terminate();
        }
        state_.lifecycle_.complete_run();
    }

    [[nodiscard]] bool stop_requested() const {
        std::lock_guard lock(state_.mutex_);
        return state_.lifecycle_.stop_requested();
    }

    void start_signal_handling() {
        if (state_.options_.server_.process_signal_handlers_ == process_signal_handler_policy::install) {
            signals_.emplace(owner_);
        }
    }

    void publish_ready_workers() {
        std::lock_guard lock(state_.mutex_);
        state_.runtime_->workers_ready_ = true;
    }

    void run_start_hooks() {
        for (auto& hook : state_.on_start_hooks_) {
            if (stop_requested()) {
                return;
            }
            hook();
        }
    }

    void wait_for_stop() {
        std::unique_lock lock(state_.mutex_);
        if (!state_.lifecycle_.mark_running()) {
            return;
        }
        state_.lifecycle_changed_.wait(lock, [this] { return state_.lifecycle_.stop_requested(); });
    }

    void stop_signal_handling() noexcept {
        if (signals_.has_value()) {
            signals_->stop();
        }
    }

    // Graceful stop of a serving app: ingress stops accepting, every worker
    // stops admitting requests, and in-flight requests may finish until the
    // shared drain deadline. A member failure has already stopped the group,
    // which closes every connection at once instead.
    void drain() noexcept {
        auto& runtime = *state_.runtime_;
        if (state_.options_.server_.drain_timeout_.count() == 0 || runtime.group_.stop_requested()) {
            return;
        }
        const auto deadline = worker_timer_deadline_after(state_.options_.server_.drain_timeout_);
        runtime.ingress_->close_admission();
        for (auto& slot : runtime.workers_) {
            slot.runtime_->request_drain();
        }
        for (auto& slot : runtime.workers_) {
            if (!slot.runtime_->wait_until_drained(deadline)) {
                return;
            }
        }
    }

    void retire_runtime() noexcept {
        std::unique_ptr<detail::app_runtime_graph, pmr_object_deleter<detail::app_runtime_graph>>
            retired(nullptr, pmr_object_deleter<detail::app_runtime_graph>{runtime_resource_});
        {
            std::lock_guard lock(state_.mutex_);
            retired = std::move(state_.runtime_);
        }
        retired.reset();
        {
            std::lock_guard lock(state_.mutex_);
            state_.lifecycle_.complete_run();
        }
    }

    application& owner_;
    detail::app_state& state_;
    std::pmr::memory_resource* runtime_resource_;
    // Constructed only for process_signal_handler_policy::install.
    std::optional<process_signal_handling> signals_;
};

}  // namespace

void detail::run_app(application& app, app_state& state_value) {
    app_run_coordinator(app, state_value).run();
}

void detail::request_runtime_stop(app_state& state_value) noexcept {
    if (state_value.runtime_ != nullptr) {
        state_value.runtime_->group_.request_stop();
    }
}

}  // namespace ruvia
