#include "ruvia/web/testing.h"

#include <bit>
#include <chrono>
#include <exception>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "ruvia/core/event_loop_pool.h"
#include "ruvia/core/memory/memory_pool.h"
#include "ruvia/http/http_accept_encoding.h"
#include "ruvia/http/http_ascii.h"
#include "ruvia/http/http_protocol_error.h"
#include "ruvia/web/detail/controller/controller_runtime.h"
#include "ruvia/web/dotenv.h"

#include "context/context_services.h"
#include "http/http_protocol_error_info.h"
#include "integration/worker_capabilities.h"
#include "router/route_table.h"
#include "router/router_impl.h"
#include "server/http_request_admission.h"
#include "server/http_server_options.h"
#include "server/http_server_options_validation.h"
#include "server/response_file_source.h"
#include "server/worker_codecs.h"
#include "util/config_validation.h"

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
    detail::worker_codecs codecs_{memory_.resource()};
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

    impl_type() {
        options_.codecs_ = &codecs_;
    }

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
                    .trusted_proxies_ =
                        options_.trusted_proxies_.empty() ? nullptr : &options_.trusted_proxies_,
                    .precompressed_static_files_ = options_.compression_.has_value(),
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

    [[nodiscard]] task<test_response> execute_request(const test_request& request) {
        const auto request_start = std::chrono::steady_clock::now();
        request_memory request_memory_value(memory_);
        std::pmr::vector<http_header_view> headers(request_memory_value.resource());
        headers.reserve(request.headers_.size() + (!request.cookies_.empty() ? 1U : 0U));
        for (const auto& [name, value] : request.headers_) {
            headers.emplace_back(name, value);
        }
        if (!request.cookies_.empty()) {
            headers.emplace_back("Cookie", request.cookies_);
        }
        auto parsed_request = make_parsed_http_request(request.method_, request.target_, headers,
            std::as_bytes(std::span(request.body_.data(), request.body_.size())),
            request_memory_value.resource());
        const auto* parse_error = std::get_if<http_parse_error>(&parsed_request);
        auto parsed_value = parse_error == nullptr
                                ? std::move(std::get<http_request>(parsed_request))
                                : std::get<http_request>(make_parsed_http_request(
                                      "GET", "/", {}, {}, request_memory_value.resource()));

        const auto& routes_value = *routes_;
        const auto resolution = routes_value.resolve(parsed_value);
        const auto services = capabilities_->make_context_services(stop_token_);

        // A malformed request is rejected before admission, with no coding
        // promise, exactly as a server answers a malformed head.
        std::optional<detail::http_request_admission> admission;
        http_response response(request_memory_value.resource());
        if (parse_error != nullptr) {
            response = co_await detail::respond_http_request_rejection(routes_value, parsed_value,
                request_memory_value,
                detail::http_request_rejection::error(detail::copy_http_protocol_error_info(
                    request_memory_value.resource(), http_parse_protocol_error(*parse_error))),
                services);
        } else {
            admission.emplace(co_await detail::admit_and_dispatch_buffered_request(routes_value,
                {
                    .request_ = parsed_value,
                    .resolution_ = resolution,
                    .coding_ = http_response_coding_selection_for(parsed_value),
                    .resource_ = request_memory_value.resource(),
                    .start_ = request_start,
                },
                detail::http_request_admission_policy::server(options_), services,
                request_memory_value, response));
        }
        const auto& request_services = admission ? admission->services() : services;
        const auto tail = admission ? admission->tail()
                                    : detail::http_response_tail(parsed_value,
                                          detail::http_response_coding_policy::disabled(), {}, request_start);
        const auto preparation = tail.prepare(response, options_);
        const auto prepared = preparation.ready() != nullptr
                                  ? *preparation.ready()
                                  : co_await tail.finish_prepare(
                                        preparation, response, options_, routes_value, request_memory_value,
                                        request_services);

        // Copy everything out while the request arena is still alive.
        dispatch_response result_value(process_resource());
        result_value.status_ = response.status();
        result_value.headers_.reserve(response.headers().size());
        for (const auto& header : response.headers()) {
            result_value.headers_.emplace_back(header.name(), header.value(), process_resource());
        }
        // and content-forbidden statuses, so the facade must not surface one
        // either. Writer-synthesized fields (Content-Length, Date, Connection)
        // are framing concerns and stay absent here.
        if (!prepared.write_plan_.body_suppressed()) {
            if (response.file_body() || response.has_multipart_file_body()) {
                detail::response_file_source source(response,
                    {.pool_ = nullptr,
                        .worker_ = worker_,
                        .stop_ = stop_token_,
                        .resource_ = request_memory_value.resource()});
                for (;;) {
                    const auto chunk = co_await source.next();
                    if (chunk.kind() == detail::response_body_chunk::kind_type::end) {
                        break;
                    }
                    if (chunk.kind() == detail::response_body_chunk::kind_type::failed) {
                        throw std::runtime_error("failed to read file response in test_app");
                    }
                    result_value.body_.append(chunk.bytes());
                }
            } else {
                const auto body = response.body_bytes();
                result_value.body_.assign(reinterpret_cast<const char*>(body.data()), body.size());
            }
        }
        co_return result_value;
    }
};

test_app::test_app()
    : impl_(make_pmr_object<impl_type>(process_resource())) {}

test_app::~test_app() = default;

test_app& test_app::server(server_config config) {
    impl_->require_configurable();
    impl_->options_.set_server(config);
    return *this;
}

test_app& test_app::compression(compression_config config) {
    impl_->require_configurable();
    impl_->options_.set_compression(std::move(config));
    return *this;
}

test_app& test_app::compression(std::nullptr_t) {
    impl_->require_configurable();
    impl_->options_.compression_.reset();
    return *this;
}

test_app& test_app::cors(cors_config config) {
    impl_->require_configurable();
    impl_->options_.set_cors(config);
    return *this;
}

test_app& test_app::cors(std::nullptr_t) {
    impl_->require_configurable();
    impl_->options_.cors_.reset();
    return *this;
}

test_app& test_app::rate_limit(rate_limit_config config) {
    impl_->require_configurable();
    impl_->options_.set_rate_limit(config);
    return *this;
}

test_app& test_app::rate_limit(std::nullptr_t) {
    impl_->require_configurable();
    impl_->options_.default_rate_limit_per_worker_.reset();
    return *this;
}

test_app& test_app::trusted_proxies(trusted_proxy_config config) {
    impl_->require_configurable();
    impl_->options_.set_trusted_proxies(config);
    return *this;
}

test_app& test_app::trusted_proxies(std::nullptr_t) {
    impl_->require_configurable();
    impl_->options_.trusted_proxies_ = detail::trusted_proxy_set(process_resource());
    return *this;
}

test_app& test_app::on_error(http_error_handler handler) {
    impl_->require_configurable();
    impl_->configuration_.on_error(std::move(handler));
    return *this;
}

test_app& test_app::on_not_found(http_not_found_handler handler) {
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

    return impl_->event_loop_.start(impl_->execute_request(request)).get();
}

}  // namespace ruvia
