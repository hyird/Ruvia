#include <algorithm>
#include <charconv>
#include <exception>
#include <memory_resource>
#include <optional>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

#include "ruvia/core/memory/pmr_object.h"
#include "ruvia/core/scoped_operation.h"
#include "ruvia/http/http_request_body_error.h"
#include "ruvia/web/detail/app/app_configuration.h"
#include "ruvia/web/detail/router/prefix_fallback.h"
#include "ruvia/web/request_stream.h"
#include "ruvia/web/router.h"

#include "body/http_request_body_facade.h"
#include "context/context_access.h"
#include "ratelimit/rate_limiter.h"
#include "router/router_impl.h"

namespace ruvia::detail {
namespace {

// The transport still owns framing, timeouts and the stream limit. This view
// can only tighten its wire-byte budget and buffers lazily, never from a peer's
// advertised length. Storage counts against the same inbound budget.
class routed_body final {
public:
    routed_body(body_reader* source, std::optional<std::size_t> limit, std::pmr::memory_resource* resource)
        : source_(source),
          limit_(limit),
          bytes_(resource) {}

    task<std::optional<std::span<const std::byte>>> read() {
        if (failure_) {
            std::rethrow_exception(failure_);
        }
        // An Upgrade/CONNECT request has no HTTP content; subsequent bytes
        // belong exclusively to the transport's tunnel acceptance operation.
        if (source_ == nullptr) {
            co_return std::nullopt;
        }
        try {
            auto chunk = co_await source_->read();
            if (chunk && limit_) {
                if (chunk->size() > *limit_ - consumed_) {
                    throw http_request_body_protocol_error(http_request_body_failure::too_large);
                }
                consumed_ += chunk->size();
            }
            co_return chunk;
        } catch (...) {
            failure_ = std::current_exception();
            throw;
        }
    }

    task<std::string_view> read_all() {
        while (auto chunk = co_await read()) {
            bytes_.append(reinterpret_cast<const char*>(chunk->data()), chunk->size());
        }
        co_return std::string_view(bytes_);
    }

    task<void> discard() {
        while (co_await read()) {
        }
    }

private:
    body_reader* source_;
    std::optional<std::size_t> limit_;
    std::size_t consumed_{0};
    std::pmr::string bytes_;
    std::exception_ptr failure_;
};

}  // namespace

class router_state final {
public:
    explicit router_state(worker_handle worker)
        : worker_(std::move(worker)) {
        if (!worker_.valid()) {
            throw std::invalid_argument("router requires a valid worker");
        }
    }

    void require_current() const {
        if (!worker_.is_current()) {
            throw std::logic_error("router operation must run on its owner worker");
        }
    }

    void require_editable() const {
        require_current();
        if (phase_ != phase::editable) {
            throw std::logic_error("router configuration is sealed or being frozen");
        }
    }

    struct registration final {
        std::pmr::string method_;
        std::pmr::string path_;
        std::variant<router_handler, router_stream_handler> handler_;
        std::pmr::vector<controller_middleware_descriptor> modifiers_;

        template <typename handler_type>
        registration(std::string_view method, std::string_view path, handler_type handler,
            std::span<const controller_middleware_descriptor> modifiers)
            : method_(method, process_resource()),
              path_(path, process_resource()),
              handler_(std::move(handler)),
              modifiers_(modifiers.begin(), modifiers.end(), process_resource()) {
            if (!is_valid_http_method_token(method)) {
                throw std::invalid_argument("route method must be a valid HTTP token");
            }
            if (!std::visit([](const auto& value) { return static_cast<bool>(value); }, handler_)) {
                throw std::invalid_argument("route handler must not be empty");
            }
        }
    };

    void freeze() {
        require_editable();
        phase_ = phase::freezing;
        try {
            detail::router builder;
            std::pmr::vector<void*> targets(process_resource());
            targets.reserve(registrations_.size());
            for (const auto& route : registrations_) {
                const auto method = classify_http_method(route.method_);
                const auto token = method == http_known_method::unknown ? std::string_view(route.method_) : std::string_view{};
                const auto body_mode = std::ranges::any_of(route.modifiers_, [](const auto& value) { return value.stream_request_body(); })
                                           ? request_body_mode::stream
                                           : request_body_mode::buffered;
                std::visit([&](const auto& handler) {
                    const auto view = callback_access::ref(handler);
                    auto endpoint = [&] {
                        if constexpr (std::same_as<std::remove_cvref_t<decltype(handler)>, router_handler>) {
                            return route_endpoint::buffered(callback_access::invoke(view), body_mode);
                        } else {
                            return route_endpoint::response_stream(callback_access::invoke(view), body_mode);
                        }
                    }();
                    builder.register_owned_endpoint(targets.size(), method, token, route.path_, std::move(endpoint), {}, route.modifiers_);
                    targets.push_back(callback_access::target(view));
                },
                    route.handler_);
            }
            configuration_.apply(builder);
            auto plan = builder.compile();
            auto table = make_pmr_object<route_table>(process_resource(), *plan, std::span<void* const>(targets));
            auto limiter = table->has_route_rate_limit()
                               ? make_pmr_object<rate_limiter>(process_resource(), std::nullopt,
                                     route_rate_limit_presence::present, default_rate_limit_capacity_per_worker)
                               : std::unique_ptr<rate_limiter, pmr_object_deleter<rate_limiter>>{};
            plan_ = std::move(plan);
            table_ = std::move(table);
            limiter_ = std::move(limiter);
            phase_ = phase::frozen;
        } catch (...) {
            phase_ = phase::editable;
            throw;
        }
    }

    static task<void> dispatch(std::shared_ptr<router_state> self, request_stream& stream) {
        self->require_current();
        if (self->phase_ != phase::frozen || stream.worker().id() != self->worker_.id()) {
            throw std::logic_error("dispatch requires a frozen router on the request's worker");
        }
        auto& context_value = stream.context_;
        if (stream.routed_ || stream.response_.committed() || context_access::output_committed(context_value) ||
            context_access::has_response(context_value)) {
            throw std::logic_error("request routing or response selection has already begun");
        }
        stream.routed_ = true;
        const auto& table = *self->table_;
        const auto& request = stream.request();
        auto& memory = context_access::memory(context_value);
        const auto resolution = table.resolve(request);
        const auto* resolved = resolution.resolved();
        context_access::route_binding binding(context_value,
            resolved ? resolved->route().path() : std::string_view{},
            resolved ? resolved->route().param_names() : std::span<const std::string_view>{},
            resolved ? resolved->match().values() : std::span<const std::string_view>{},
            resolved ? reinterpret_cast<std::uintptr_t>(&resolved->route()) : 0);
        auto& services = context_access::services(context_value);
        services = services.with_request_stream({});
        services.bind_routes(table);
        if (self->limiter_) {
            services.bind_route_rate_limiter(*self->limiter_);
        }
        if (resolved == nullptr) {
            auto buffered_services = services;
            buffered_services.response_output() = {};
            http_response response(context_value.arena());
            co_await table.dispatch_buffered_response(request, resolution, memory, document_root_binding::none(),
                buffered_services, static_file_selection_mode::identity_only, response);
            stream.respond(std::move(response));
            co_return;
        }

        const auto& route = resolved->route();
        services.bind_route_body_limit(route.max_request_body_bytes());
        const auto* buffered = route.endpoint().buffered();
        const auto mode = buffered ? buffered->request_body_mode() : route.endpoint().response_stream()->request_body_mode();
        const auto limit = mode == request_body_mode::buffered
                               ? std::optional<std::size_t>(services.max_decoded_request_body_bytes())
                           : route.max_request_body_bytes() != 0 ? std::optional<std::size_t>(route.max_request_body_bytes())
                                                                 : std::nullopt;
        const auto request_services = services;
        auto* const body_source = stream.tunnel_requested() ? nullptr : &stream.body();
        std::exception_ptr failure;
        try {
            // The transport already validated and normalized this field. Do not
            // send 100 Continue or run application code for an oversized body.
            if (limit) {
                if (const auto length = request.header("Content-Length")) {
                    std::size_t declared = 0;
                    const auto parsed = std::from_chars(length->data(), length->data() + length->size(), declared);
                    if (parsed.ec == std::errc::result_out_of_range || (parsed.ec == std::errc{} && declared > *limit)) {
                        throw http_request_body_protocol_error(http_request_body_failure::too_large);
                    }
                }
            }
            routed_body body(body_source, limit,
                services.inbound_buffer_pool() != nullptr ? services.inbound_buffer_pool() : process_resource());
            auto reader = make_body_reader_facade(body);
            auto loader = make_request_body_loader_facade(body);
            if (mode == request_body_mode::stream) {
                services.bind_streaming_request_body(reader);
            } else {
                services.bind_lazy_request_body(loader);
            }
            if (buffered != nullptr) {
                const auto output = services.response_output();
                services.response_output() = {};
                auto response = co_await table.dispatch_bound_buffered(*resolved, context_value);
                services.response_output() = output;
                context_access::set_response(context_value, std::move(response));
            } else {
                auto response = co_await table.dispatch_bound_stream(*resolved, context_value, resolved->stream_handler());
                if (response) {
                    context_access::set_response(context_value, std::move(*response));
                }
            }
            co_return;
        } catch (...) {
            failure = std::current_exception();
        }
        if (stream.response_.committed() || context_access::output_committed(context_value)) {
            std::rethrow_exception(failure);
        }
        // Body adapters above have retired. The request-only error path must
        // not retain their borrowed views while constructing its fallback.
        services = request_services;
        auto response = co_await table.handle_exception(request, memory, failure, services);
        context_access::set_response(context_value, std::move(response));
    }

    worker_handle worker_;
    app_configuration configuration_;
    std::pmr::vector<registration> registrations_{process_resource()};

private:
    enum class phase { editable,
        freezing,
        frozen };
    phase phase_{phase::editable};
    compiled_route_plan_ptr_type plan_;
    std::unique_ptr<route_table, pmr_object_deleter<route_table>> table_;
    std::unique_ptr<rate_limiter, pmr_object_deleter<rate_limiter>> limiter_;
};

}  // namespace ruvia::detail

namespace ruvia {

router::router(worker_handle worker)
    : state_(std::allocate_shared<detail::router_state>(std::pmr::polymorphic_allocator<detail::router_state>(process_resource()), std::move(worker))) {}
router::~router() = default;

router& router::add_buffered(std::string_view method, std::string_view path, router_handler handler,
    std::span<const detail::controller_middleware_descriptor> modifiers) {
    state_->require_editable();
    state_->registrations_.emplace_back(method, path, std::move(handler), modifiers);
    return *this;
}

router& router::add_stream(std::string_view method, std::string_view path, router_stream_handler handler,
    std::span<const detail::controller_middleware_descriptor> modifiers) {
    state_->require_editable();
    state_->registrations_.emplace_back(method, path, std::move(handler), modifiers);
    return *this;
}

router& router::add_middleware(detail::owned_middleware_descriptor descriptor) {
    state_->require_editable();
    state_->configuration_.add_middleware(std::move(descriptor));
    return *this;
}

router& router::add_scoped_middleware(std::string_view prefix, detail::owned_middleware_descriptor descriptor) {
    return add_middleware(std::move(descriptor).scoped_to(detail::normalize_fallback_prefix(prefix)));
}

router& router::on_error(http_error_handler handler) & {
    state_->require_editable();
    state_->configuration_.on_error(std::move(handler));
    return *this;
}

router& router::on_not_found(http_not_found_handler handler) & {
    state_->require_editable();
    state_->configuration_.on_not_found(std::move(handler));
    return *this;
}

router& router::on_error(scoped_error_handler_options options) & {
    state_->require_editable();
    state_->configuration_.on_error(std::move(options));
    return *this;
}

router& router::on_not_found(scoped_not_found_handler_options options) & {
    state_->require_editable();
    state_->configuration_.on_not_found(std::move(options));
    return *this;
}

void router::freeze() & {
    state_->freeze();
}

task<void> router::dispatch(request_stream& stream) & {
    return make_scoped_operation(stream.operation_scope_, detail::router_state::dispatch(state_, stream));
}

}  // namespace ruvia
