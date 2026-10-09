#include "app/app_run_coordinator.h"

#include <algorithm>
#include <csignal>
#include <cstdint>
#include <exception>
#include <memory>
#include <memory_resource>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
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

void stage_worker_quic(void* object, detail::http3_datagram_channel& channel,
    asio::ip::udp::endpoint endpoint, quic_cid_partition partition) {
    static_cast<detail::web_worker_runtime*>(object)->stage_quic(channel, endpoint, partition);
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

[[nodiscard]] std::unique_ptr<detail::router, detail::pmr_object_deleter<detail::router>>
build_worker_router(const detail::app_state& state_value, std::pmr::memory_resource* runtime_resource,
    detail::controller_store& controllers,
    std::span<const detail::controller_registrar_type> controller_registrars,
    const detail::compiled_route_plan* compiled_plan) {
    auto router_value = detail::make_pmr_object<detail::router>(runtime_resource);
    detail::register_controllers(*router_value, controllers, controller_registrars);
    auto& routes_value = detail::router_impl::from(*router_value);
    state_value.configuration_.apply(routes_value, runtime_resource);
    routes_value.finalize(compiled_plan);
    return router_value;
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

        std::exception_ptr primary_failure;
        try {
            if (!stop_requested()) {
                start_signal_handling();
                start_workers();
                run_start_hooks();
                start_serving();
                wait_for_stop();
            }
        } catch (...) {
            primary_failure = std::current_exception();
            owner_.stop();
        }

        stop_workers();
        stop_signal_handling();
        invoke_stop_hooks(state_);
        const auto failure = join_workers();
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
            detail::make_pmr_object<detail::app_runtime_graph>(runtime_resource_, runtime_resource_);
        auto prepared_options = state_.options_;
        prepared_options.env_ = &state_.env_;
        prepared_options.failure_ = detail::worker_failure_sink{
            .target_ = &owner_,
            .invoke_ =
                [](void* target, const std::exception_ptr&) noexcept {
                    static_cast<application*>(target)->stop();
                },
        };

        std::unique_ptr<static_root, detail::pmr_object_deleter<static_root>> configured_document_root(
            nullptr, detail::pmr_object_deleter<static_root>{runtime_resource_});
        if (state_.document_root_config_.has_value()) {
            const auto document_root_path =
                ruvia::make_path_from_native_path(state_.document_root_config_->root_);
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
                detail::make_pmr_object<blocking_pool>(runtime_resource_, *state_.blocking_pool_);
            prepared_options.blocking_pool_ = runtime->blocking_pool_.get();
        }

        const auto validated_configuration =
            detail::validate_http_server_configuration(state_.listeners_, std::move(prepared_options));

        runtime->workers_.reserve(state_.worker_count_);
        for (std::size_t i = 0; i < state_.worker_count_; ++i) {
            detail::controller_store controllers;
            auto router_value = build_worker_router(state_, runtime_resource_, controllers,
                controller_registrars, runtime->route_plan_.get());
            auto& routes_value = detail::router_impl::from(*router_value);
            if (runtime->route_plan_ == nullptr) {
                runtime->route_plan_ = routes_value.release_compiled_plan();
            }
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
            auto worker_value = detail::make_pmr_object<detail::web_worker_runtime>(
                runtime_resource_, validated_configuration, routes_value.route_table(), capabilities);
            worker_value->prepare();
            runtime->workers_.emplace_back(
                std::move(controllers), std::move(router_value), std::move(worker_value));
        }

        const bool has_http3 = std::ranges::any_of(validated_configuration.listeners(),
            [](const detail::http_server_listener_definition& listener_value) {
                return listener_value.http3_.has_value();
            });
        runtime->acceptor_targets_.reserve(runtime->workers_.size());
        for (auto& slot : runtime->workers_) {
            auto* target = slot.runtime_.get();
            runtime->acceptor_targets_.push_back({
                .submission_ = target->network_submission(),
                .object_ = target,
                .available_ = &acceptor_target_available,
                .accept_ = &acceptor_target_accept,
                .stage_quic_ = has_http3 ? &stage_worker_quic : nullptr,
            });
        }
        runtime->ingress_ = std::make_unique<detail::acceptor>(
            validated_configuration.listeners(), runtime->acceptor_targets_, &owner_, &acceptor_failed);
        runtime->ingress_->prepare();

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

    void start_workers() {
        // Bind ingress and stage the datagram channels before workers construct
        // their own QUIC/TLS and HTTP/3 state.
        {
            std::lock_guard lock(state_.mutex_);
            if (state_.lifecycle_.stop_requested()) {
                return;
            }
            state_.runtime_->ingress_->launch();
        }
        state_.runtime_->ingress_->wait_until_ready();
        state_.runtime_->ingress_->rethrow_failure();
        if (stop_requested()) {
            return;
        }

        for (auto& worker_value : state_.runtime_->workers_) {
            {
                std::lock_guard lock(state_.mutex_);
                if (state_.lifecycle_.stop_requested()) {
                    return;
                }
                worker_value.runtime_->launch();
            }
        }
        for (auto& worker_value : state_.runtime_->workers_) {
            if (stop_requested()) {
                return;
            }
            worker_value.runtime_->wait_until_ready();
        }
    }

    void start_serving() {
        for (auto& worker_value : state_.runtime_->workers_) {
            if (stop_requested()) {
                return;
            }
            worker_value.runtime_->request_serve();
        }
        for (auto& worker_value : state_.runtime_->workers_) {
            if (stop_requested()) {
                return;
            }
            if (!worker_value.runtime_->wait_until_serving()) {
                if (stop_requested()) {
                    return;
                }
                throw std::runtime_error("web worker stopped before the application began serving");
            }
        }
        if (!stop_requested()) {
            state_.runtime_->ingress_->request_serve();
            if (!state_.runtime_->ingress_->wait_until_serving()) {
                state_.runtime_->ingress_->rethrow_failure();
                if (stop_requested()) {
                    return;
                }
                throw std::runtime_error("acceptor stopped before the application began serving");
            }
        }
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

    void stop_workers() noexcept {
        if (state_.runtime_->ingress_) {
            state_.runtime_->ingress_->stop();
        }
        for (auto& worker_value : state_.runtime_->workers_) {
            try {
                worker_value.runtime_->stop_admission();
            } catch (...) {
                ruvia::report_unhandled_failure("web worker stop", std::current_exception());
            }
        }
    }

    void stop_signal_handling() noexcept {
        signal_runtime_.request_stop();
        signal_runtime_.join();
    }

    [[nodiscard]] std::exception_ptr join_workers() noexcept {
        std::exception_ptr first_failure;
        // The acceptor joins after datagram publishers acknowledge closure.
        // Workers retain their own protocol state until their task scopes join.
        bool acceptor_quiesced = state_.runtime_->ingress_ == nullptr;
        if (state_.runtime_->ingress_) {
            try {
                state_.runtime_->ingress_->join();
                acceptor_quiesced = true;
                state_.runtime_->ingress_->rethrow_failure();
            } catch (...) {
                if (!acceptor_quiesced) {
                    std::terminate();
                }
                first_failure = std::current_exception();
            }
        }
        if (!acceptor_quiesced) {
            std::terminate();
        }
        for (auto& worker_value : state_.runtime_->workers_) {
            try {
                worker_value.runtime_->finalize_after_network_quiesced();
                worker_value.runtime_->join();
            } catch (...) {
                if (first_failure == nullptr) {
                    first_failure = std::current_exception();
                } else {
                    ruvia::report_unhandled_failure(
                        "additional web worker failure", std::current_exception());
                }
            }
        }
        return first_failure;
    }

    void retire_runtime() noexcept {
        std::unique_ptr<detail::app_runtime_graph, detail::pmr_object_deleter<detail::app_runtime_graph>>
            retired(nullptr, detail::pmr_object_deleter<detail::app_runtime_graph>{runtime_resource_});
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

}  // namespace ruvia
