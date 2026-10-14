#include "ruvia/web/testing.h"

#include <chrono>
#include <exception>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "ruvia/core/event_loop_pool.h"
#include "ruvia/core/memory/memory_pool.h"
#include "ruvia/http/http_accept_encoding.h"
#include "ruvia/http/http_ascii.h"
#include "ruvia/http/http_parse_error.h"
#include "ruvia/web/detail/controller/controller_runtime.h"
#include "ruvia/web/dotenv.h"

#include "context/context_services.h"
#include "http/http_protocol_error_info.h"
#include "integration/worker_capabilities.h"
#include "router/route_table.h"
#include "router/router_impl.h"
#include "server/http_request_admission.h"
#include "server/http_server_options.h"

namespace ruvia {

// test_app drives the production worker bring-up (worker_capabilities
// start/stop) and the production per-request chain (admission, dispatch, the
// response tail) on one core event loop. Only the transport is absent.
struct test_app::impl_type final {
    enum class lifecycle_type { configuring,
        initializing,
        ready,
        failed };

    detail::app_configuration configuration_{detail::registration_resource()};
    worker_memory memory_;
    detail::controller_store controllers_;
    detail::compiled_route_plan_ptr_type route_plan_{nullptr, pmr_object_deleter<detail::compiled_route_plan>{}};
    std::optional<detail::route_table> routes_;
    env env_;
    // The defaults a server starts from: no listener-specific policy applies.
    detail::http_server_options options_{};
    event_loop_pool event_loop_pool_{{.loop_count_ = 1}};
    event_loop event_loop_{event_loop_pool_.loop(0)};
    worker_handle worker_{event_loop_.handle()};
    stop_source stop_source_;
    stop_token stop_token_{stop_source_.token()};
    std::optional<detail::worker_capabilities> capabilities_;
    lifecycle_type lifecycle_{lifecycle_type::configuring};
    std::exception_ptr startup_failure_{};

    ~impl_type() {
        if (lifecycle_ == lifecycle_type::ready) {
            stop_source_.request_stop();
            try {
                event_loop_.start(capabilities_->stop()).get();
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
            controllers_ = detail::controller_store(controller_registrars);
            route_plan_ = detail::compile_route_plan(
                memory_.resource(), controller_registrars, controllers_, configuration_);
            routes_.emplace(memory_.resource(), *route_plan_, controllers_);

            capabilities_.emplace(event_loop_.io_context(), worker_, memory_.resource(),
                detail::worker_capability_definitions{.worker_states_ = configuration_.worker_states()},
                detail::worker_capability_options{
                    .default_rate_limit_ = options_.default_rate_limit_per_worker_,
                    .route_rate_limits_ = routes_->has_route_rate_limit()
                                              ? detail::route_rate_limit_presence::present
                                              : detail::route_rate_limit_presence::absent,
                    .rate_limit_capacity_ = options_.rate_limit_capacity_per_worker_,
                    .max_decoded_body_bytes_ = options_.server_.max_buffered_body_bytes_,
                    .http_client_result_budget_ = options_.server_.http_client_result_budget_,
                    .env_ = &env_,
                });
            event_loop_pool_.start();
            event_loop_.start(capabilities_->start()).get();
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

test_app& test_app::use_middleware(detail::owned_middleware_descriptor descriptor) {
    impl_->require_configurable();
    impl_->configuration_.add_middleware(std::move(descriptor));
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
        const auto request_start = std::chrono::steady_clock::now();
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

        const auto& options = impl_->options_;
        const auto& routes_value = *impl_->routes_;
        const auto resolution = routes_value.resolve(parsed_value);
        const auto services = impl_->capabilities_->make_context_services(impl_->stop_token_);

        // A malformed request is rejected before admission, with no coding
        // promise, exactly as a server answers a malformed head.
        std::optional<detail::http_request_admission> admission;
        std::optional<detail::http_request_rejection> rejection;
        if (parse_error.has_value()) {
            rejection = detail::http_request_rejection::error(detail::copy_http_protocol_error_info(
                request_memory_value.resource(), http_parse_protocol_error(*parse_error)));
        } else {
            admission.emplace(detail::admit_http_request(
                {
                    .request_ = parsed_value,
                    .resolution_ = resolution,
                    .coding_ = http_response_coding_selection_for(parsed_value),
                    .resource_ = request_memory_value.resource(),
                    .start_ = request_start,
                },
                detail::http_request_admission_policy::server(options), services));
            if (const auto* admitted_rejection = admission->rejection()) {
                // As in the server: a rejection does not inherit the coding promise.
                rejection = *admitted_rejection;
                admission->tail().disable_response_coding();
            }
        }
        const auto& request_services = admission ? admission->services() : services;

        auto respond = [&]() -> task<http_response> {
            if (rejection) {
                co_return co_await detail::respond_http_request_rejection(
                    routes_value, parsed_value, request_memory_value, *rejection, request_services);
            }
            co_return co_await routes_value.dispatch_buffered_response(parsed_value, resolution,
                request_memory_value, detail::document_root_binding::none(), request_services,
                admission->static_file_mode());
        };
        auto response = co_await respond();

        const auto tail = admission ? admission->tail()
                                    : detail::http_response_tail(parsed_value,
                                          detail::http_response_coding_policy::disabled(), {}, request_start);
        const auto preparation = tail.prepare(response, options);
        const auto prepared = preparation.ready() != nullptr
                                  ? *preparation.ready()
                                  : co_await tail.finish_prepare(
                                        preparation, response, options, routes_value, request_memory_value,
                                        request_services);

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
        if (!prepared.write_plan_.body_suppressed()) {
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
