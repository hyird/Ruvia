#include "ruvia/web/testing.h"

#include <chrono>
#include <exception>
#include <memory>
#include <memory_resource>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "ruvia/core/connection_scanner.h"
#include "ruvia/core/event_loop_pool.h"
#include "ruvia/core/memory/memory_pool.h"
#include "ruvia/http/http_ascii.h"
#include "ruvia/http/http_limits.h"
#include "ruvia/http/http_parse_error.h"
#include "ruvia/http/http_request_body_failure.h"
#include "ruvia/http/http_request_content_semantics.h"
#include "ruvia/web/detail/controller/controller_runtime.h"
#include "ruvia/web/detail/router/prefix_fallback.h"
#include "ruvia/web/dotenv.h"

#include "context/context_services.h"
#include "integration/worker_capabilities.h"
#include "router/router.h"
#include "router/router_impl.h"
#include "server/request_deadline.h"

namespace ruvia {

namespace {

task<void> start_test_worker(
    ruvia::connection_scanner& scanner, detail::worker_capabilities& capabilities) {
    try {
        capabilities.initialize_worker_state();
        scanner.start();
        co_await capabilities.connect();
    } catch (...) {
        scanner.stop();
        capabilities.shutdown_worker_state();
        throw;
    }
}

task<void> stop_test_worker(
    ruvia::connection_scanner& scanner, detail::worker_capabilities& capabilities) {
    scanner.stop();
    scanner.close_all();
    capabilities.close_now();
    try {
        co_await capabilities.join();
    } catch (...) {
        capabilities.shutdown_worker_state();
        throw;
    }
    capabilities.shutdown_worker_state();
}

}  // namespace

struct test_app::impl_type final {
    enum class lifecycle_type { configuring,
        initializing,
        ready,
        failed };

    detail::app_configuration configuration_{detail::registration_resource()};
    detail::router router_;
    detail::controller_store controllers_;
    worker_memory memory_;
    env env_;
    event_loop_pool event_loop_pool_{{.loop_count_ = 1}};
    event_loop event_loop_{event_loop_pool_.loop(0)};
    worker_handle worker_{event_loop_.handle()};
    stop_source stop_source_;
    stop_token stop_token_{stop_source_.token()};
    std::optional<ruvia::connection_scanner> connection_scanner_;
    std::optional<detail::worker_capabilities> capabilities_;
    lifecycle_type lifecycle_{lifecycle_type::configuring};
    std::exception_ptr startup_failure_{};

    ~impl_type() {
        if (lifecycle_ == lifecycle_type::ready) {
            stop_source_.request_stop();
            try {
                event_loop_.start(stop_test_worker(*connection_scanner_, *capabilities_)).get();
            } catch (...) {
                std::terminate();
            }
        }
        event_loop_pool_.stop();
        event_loop_pool_.join();
    }

    void require_configurable() const {
        if (lifecycle_ != lifecycle_type::configuring) {
            throw std::logic_error("test_app must be configured before its first request()");
        }
    }

    void finalize() {
        if (lifecycle_ == lifecycle_type::ready) {
            return;
        }
        if (lifecycle_ == lifecycle_type::initializing) {
            throw std::logic_error("test_app request() cannot be reentered during startup");
        }
        if (lifecycle_ == lifecycle_type::failed) {
            std::rethrow_exception(startup_failure_);
        }
        lifecycle_ = lifecycle_type::initializing;

        try {
            const auto controller_registrars = detail::seal_controller_registrars();
            detail::register_controllers(router_, controllers_, controller_registrars);
            auto& routes_value = detail::router_impl::from(router_);
            configuration_.apply(routes_value, memory_.resource());
            routes_value.finalize();

            connection_scanner_.emplace(worker_, ruvia::connection_scanner_options{});
            capabilities_.emplace(event_loop_.io_context(), worker_, memory_.resource(),
                detail::worker_capability_definitions{.worker_states_ = configuration_.worker_states()},
                detail::worker_capability_options{
                    .route_rate_limits_ = routes_value.route_table().has_route_rate_limit()
                                              ? detail::route_rate_limit_presence::present
                                              : detail::route_rate_limit_presence::absent,
                    .rate_limit_capacity_ = 1024,
                    .env_ = &env_,
                });
            event_loop_pool_.start();
            event_loop_.start(start_test_worker(*connection_scanner_, *capabilities_)).get();
            lifecycle_ = lifecycle_type::ready;
        } catch (...) {
            startup_failure_ = std::current_exception();
            lifecycle_ = lifecycle_type::failed;
            throw;
        }
    }
};

test_app::test_app()
    : impl_(std::make_unique<impl_type>()) {}

test_app::~test_app() = default;

test_app& test_app::on_error(http_error_handler_type handler) {
    impl_->require_configurable();
    impl_->configuration_.on_error(std::move(handler));
    return *this;
}

test_app& test_app::on_not_found(http_not_found_handler_type handler) {
    impl_->require_configurable();
    impl_->configuration_.on_not_found(std::move(handler));
    return *this;
}

test_app& test_app::on_error(scoped_error_handler_options options) {
    impl_->require_configurable();
    impl_->configuration_.on_error(std::move(options));
    return *this;
}

test_app& test_app::on_not_found(scoped_not_found_handler_options options) {
    impl_->require_configurable();
    impl_->configuration_.on_not_found(std::move(options));
    return *this;
}

test_app& test_app::use_middleware(detail::controller_middleware_descriptor descriptor) {
    impl_->require_configurable();
    impl_->configuration_.add_middleware(descriptor);
    return *this;
}

test_app& test_app::use_worker_state_definition(detail::worker_state_definition definition) {
    impl_->require_configurable();
    impl_->configuration_.add_worker_state(std::move(definition));
    return *this;
}

test_response test_app::request(const test_request& request) {
    impl_->finalize();

    // Keep the request arena and every arena-backed response on the worker
    // until the response has been copied into its owning facade type.
    auto operation = [this, &request]() -> task<test_response> {
        request_memory request_memory_value(impl_->memory_);
        std::vector<http_header_view> headers;
        headers.reserve(request.headers_.size() + (!request.cookies_.empty() ? 1U : 0U));
        for (const auto& [name, value] : request.headers_) {
            headers.emplace_back(name, value);
        }
        if (!request.cookies_.empty()) {
            headers.emplace_back("Cookie", request.cookies_);
        }
        auto [parsed_value, parse_error] = make_parsed_http_request(request.method_, request.target_, headers,
            std::as_bytes(std::span(request.body_.data(), request.body_.size())),
            request_memory_value.resource());

        const auto& routes_value = detail::router_impl::from(impl_->router_).route_table();
        const auto resolution = routes_value.resolve(parsed_value);
        const auto* resolved = resolution.resolved();

        const auto services = impl_->capabilities_->make_context_services(impl_->stop_token_);

        std::optional<http_protocol_error> body_limit_error;
        if (!parse_error.has_value() && resolved != nullptr) {
            const auto route_limit = resolved->route().max_request_body_bytes();
            if (route_limit != 0 && request.body_.size() > route_limit) {
                body_limit_error = http_request_body_failure::too_large().protocol_error();
            }
        }

        auto dispatch = [&]() -> task<http_response> {
            auto request_services = services;
            std::optional<detail::request_deadline> request_deadline;
            if (!parse_error.has_value() && !body_limit_error.has_value() && resolved != nullptr &&
                resolved->route().deadline_ms() != 0) {
                request_deadline.emplace(request_services.get_stop_token());
                request_deadline->arm(request_services.worker(),
                    std::chrono::milliseconds(resolved->route().deadline_ms()));
                request_services = request_services.with_request_deadline(*request_deadline);
            }
            if (parse_error.has_value()) {
                const auto error = http_parse_protocol_error(*parse_error);
                co_return co_await routes_value.handle_error(parsed_value, request_memory_value,
                    http_error_info({.status_ = error.status(), .message_ = error.what()}),
                    request_services);
            }
            if (body_limit_error.has_value()) {
                co_return co_await routes_value.handle_error(parsed_value, request_memory_value,
                    http_error_info(
                        {.status_ = body_limit_error->status(), .message_ = body_limit_error->what()}),
                    request_services);
            }
            co_return co_await routes_value.dispatch_buffered_response(parsed_value, resolution, request_memory_value,
                detail::document_root_binding::none(), request_services);
        };
        auto response = co_await dispatch();

        // Copy everything out while the request arena is still alive.
        test_response result_value(response.status());
        result_value.headers_.reserve(response.headers().size());
        for (const auto& header : response.headers()) {
            result_value.headers_.emplace_back(std::string(header.name()), std::string(header.value()));
        }
        // Mirror wire semantics: the response writers suppress the body for HEAD
        // and content-forbidden statuses, so the facade must not surface one
        // either. Writer-synthesized fields (Content-Length, Date, Connection)
        // are framing concerns and stay absent here.
        const auto write_plan = plan_buffered_http_response_write(parsed_value.known_method(), response);
        if (!write_plan.body_suppressed()) {
            const auto body = response.body_bytes();
            result_value.body_.assign(reinterpret_cast<const char*>(body.data()), body.size());
        }
        co_return result_value;
    };
    return impl_->event_loop_.start(operation()).get();
}

std::optional<std::string_view> test_response::header(std::string_view name) const& noexcept {
    for (const auto& [header_name, value] : headers_) {
        if (http_ascii_equals_ignore_case(header_name, name)) {
            return std::string_view(value);
        }
    }
    return std::nullopt;
}

}  // namespace ruvia
