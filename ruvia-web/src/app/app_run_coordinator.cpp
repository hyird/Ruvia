#include "app/app_run_coordinator.h"

#include <algorithm>
#include <csignal>
#include <exception>
#include <memory>
#include <memory_resource>
#include <mutex>
#include <stdexcept>
#include <utility>
#include <vector>

#include <asio/signal_set.hpp>

#include "ruvia/core/failure_report.h"
#include "ruvia/core/worker_runtime.h"
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

class app_run_coordinator final {
public:
    app_run_coordinator(application& owner_value, detail::app_state& state_value)
        : owner_(owner_value),
          state_(state_value),
          runtime_resource_(detail::app_resource()),
          signal_runtime_({.queue_capacity_ = 128}),
          signals_(signal_runtime_.context().io_context()) {
        signal_runtime_.configure({
            .stop_admission_ = [this] {
                std::error_code ignored;
                signals_.cancel(ignored);
                signal_runtime_.finalize(); },
            .failure_ = [this](std::exception_ptr) noexcept { owner_.stop(); },
        });
    }

    ~app_run_coordinator() {
        stop_signal_handling();
    }

    void run() {
        begin_run();

        try {
            build_and_publish_runtime();
        } catch (...) {
            complete_unpublished_run();
            throw;
        }

        auto& group = state_.runtime_->group_;
        std::exception_ptr primary_failure;
        try {
            if (!stop_requested()) {
                start_signal_handling();
                // Every worker and the acceptor are ready before start hooks;
                // no listener accepts until every hook has succeeded.
                if (group.start()) {
                    run_start_hooks();
                    if (!stop_requested() && group.serve()) {
                        wait_for_stop();
                    }
                }
            }
        } catch (...) {
            primary_failure = std::current_exception();
            owner_.stop();
        }

        // Stop admission is distinct from final retirement: stop hooks run
        // after admission closes and before any worker is finalized.
        group.request_stop();
        stop_signal_handling();
        invoke_stop_hooks(state_);
        const auto failure = group.join();
        const auto signal_failure = signal_runtime_.failure();
        retire_runtime();

        if (primary_failure != nullptr) {
            std::rethrow_exception(primary_failure);
        }
        if (signal_failure != nullptr) {
            std::rethrow_exception(signal_failure);
        }
        if (failure != nullptr) {
            std::rethrow_exception(failure);
        }
    }

private:
    void begin_run() {
        std::lock_guard lock(state_.mutex_);
        detail::ensure_app_not_running(state_.lifecycle_.active(), "app is already running");
        if (state_.listeners_.empty()) {
            throw std::invalid_argument(
                "App::run() requires at least one listener; call App::listen() first");
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

        std::unique_ptr<static_root, pmr_object_deleter<static_root>> configured_document_root(
            nullptr, pmr_object_deleter<static_root>{runtime_resource_});
        if (state_.document_root_config_.has_value()) {
            const auto document_root_path =
                http_make_path_from_native_path(state_.document_root_config_->root_);
            configured_document_root = detail::static_root_access::make(
                runtime_resource_, document_root_path, state_.document_root_config_->static_options_);
            if (prepared_options.compression_.has_value() &&
                state_.document_root_config_->precompression_.enabled()) {
                detail::static_root_access::install_precompressed_variants(
                    *configured_document_root, nullptr, state_.document_root_config_->precompression_);
            }
            prepared_options.document_root_ =
                detail::http_server_options::document_root_type::refreshing(*configured_document_root,
                    state_.document_root_config_->runtime_, state_.document_root_config_->precompression_);
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
        runtime->workers_.reserve(state_.worker_count_);
        for (std::size_t i = 0; i < state_.worker_count_; ++i) {
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
        runtime->ingress_ = std::make_unique<detail::acceptor>(
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
        if (state_.process_signal_handlers_ != process_signal_handler_policy::install) {
            return;
        }
        add_shutdown_signals(signals_);
        signals_.async_wait([this](const std::error_code& ec, int) {
            if (!ec) {
                owner_.stop();
            }
        });
        signal_runtime_.start();
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
        signal_runtime_.request_stop();
        signal_runtime_.join();
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
    worker_runtime signal_runtime_;
    asio::signal_set signals_;
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
