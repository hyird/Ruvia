#include <algorithm>
#include <charconv>
#include <exception>
#include <memory_resource>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

#include "ruvia/core/memory/pmr_object.h"
#include "ruvia/core/scoped_operation.h"
#include "ruvia/http/http_ascii.h"
#include "ruvia/http/http_protocol_version.h"
#include "ruvia/http/http_request_body_error.h"
#include "ruvia/web/detail/app/app_configuration.h"
#include "ruvia/web/detail/integration/worker_state.h"
#include "ruvia/web/detail/router/prefix_fallback.h"
#include "ruvia/web/http_client.h"
#include "ruvia/web/request_stream.h"
#include "ruvia/web/router.h"
#ifdef RUVIA_ENABLE_DATABASE
#include "ruvia/web/db/db_handle.h"
#endif
#ifdef RUVIA_ENABLE_REDIS
#include "ruvia/web/redis/redis_handle.h"
#endif

#include "body/http_request_body_facade.h"
#include "context/context_access.h"
#include "integration/borrowed_clients.h"
#include "ratelimit/rate_limit_decision.h"
#include "ratelimit/rate_limiter.h"
#include "router/router_impl.h"
#include "util/operation_interrupt.h"

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

    task<std::optional<std::span<const std::byte>>> read(operation_options operation = {}) {
        if (failure_) {
            std::rethrow_exception(failure_);
        }
        // An Upgrade/CONNECT request has no HTTP content; subsequent bytes
        // belong exclusively to the transport's tunnel acceptance operation.
        if (source_ == nullptr) {
            co_return std::nullopt;
        }
        const operation_steps steps(operation);
        try {
            auto chunk = co_await source_->read(std::move(operation));
            if (chunk && limit_) {
                if (chunk->size() > *limit_ - consumed_) {
                    throw http_request_body_protocol_error(http_request_body_failure::too_large);
                }
                consumed_ += chunk->size();
            }
            co_return chunk;
        } catch (const std::system_error& error) {
            // An interrupted read leaves the underlying stream readable.
            if (!steps.interrupted(error)) {
                failure_ = std::current_exception();
            }
            throw;
        } catch (...) {
            failure_ = std::current_exception();
            throw;
        }
    }

    // Chunks already appended stay in bytes_ when a read is interrupted.
    task<std::string_view> read_all(operation_options operation) {
        const operation_steps steps(std::move(operation));
        while (auto chunk = co_await read(steps.next("request body read"))) {
            bytes_.append(reinterpret_cast<const char*>(chunk->data()), chunk->size());
        }
        co_return std::string_view(bytes_);
    }

    task<void> discard(operation_options operation) {
        const operation_steps steps(std::move(operation));
        while (co_await read(steps.next("request body read"))) {
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

// A router's resolution of the request it dispatches. HTTP/2 and HTTP/3 name
// a CONNECT target in :authority and an extended CONNECT protocol in
// :protocol (RFC 8441, RFC 9220); HTTP/1 routes by its request line.
[[nodiscard]] route_resolution resolve_dispatched_request(const route_table& table, const http_request& request) noexcept {
    const auto version = request.protocol_version();
    if (version != http_protocol_version::http2 && version != http_protocol_version::http3) {
        return table.resolve(request);
    }
    const auto protocol = request.protocol();
    return table.resolve(route_request_view{
        .known_method_ = request.known_method(),
        .method_ = request.method(),
        .path_ = request.path(),
        .connect_target_ = request.authority(),
        .extended_protocol_ = http_ascii_equals_ignore_case(protocol, "websocket") ? std::string_view("websocket") : protocol,
    });
}

class router_state final {
public:
    explicit router_state(worker_handle worker)
        : worker_(std::move(worker)) {
        if (!worker_.valid()) {
            throw std::invalid_argument("router requires a valid worker");
        }
    }

    router_state(const router_state&) = delete;
    router_state& operator=(const router_state&) = delete;

    // The last owner releases the state on the router's worker, which is where
    // the lent worker states were adopted.
    ~router_state() {
        if (worker_states_) {
            worker_states_->shutdown();
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

    struct tunnel_upgrade final {
        std::pmr::string protocol_;
        http_tunnel_route_config config_;
    };

    struct registration final {
        std::pmr::string method_;
        std::pmr::string path_;
        std::variant<router_handler, router_stream_handler> handler_;
        // A WebSocket or tunnel route keeps the configuration its endpoint is
        // validated and built from at freeze.
        std::variant<std::monostate, websocket_route_config, tunnel_upgrade> upgrade_;
        std::pmr::vector<controller_middleware_descriptor> modifiers_;

        template <typename handler_type, typename upgrade_type = std::monostate>
        registration(std::string_view method, std::string_view path, handler_type handler,
            std::span<const controller_middleware_descriptor> modifiers, upgrade_type upgrade = {})
            : method_(method, process_resource()),
              path_(path, process_resource()),
              handler_(std::move(handler)),
              upgrade_(std::move(upgrade)),
              modifiers_(modifiers.begin(), modifiers.end(), process_resource()) {
            if (!is_valid_http_method_token(method)) {
                throw std::invalid_argument("route method must be a valid HTTP token");
            }
            if (!std::visit([](const auto& value) { return static_cast<bool>(value); }, handler_)) {
                throw std::invalid_argument("route handler must not be empty");
            }
        }
    };

    [[nodiscard]] static route_endpoint make_endpoint(const registration& route, route_stream_handler::invoke_type invoke,
        request_body_mode body_mode) {
        if (const auto* websocket = std::get_if<websocket_route_config>(&route.upgrade_)) {
            return route_endpoint::websocket(process_resource(), invoke, *websocket);
        }
        if (const auto* tunnel = std::get_if<tunnel_upgrade>(&route.upgrade_)) {
            return route_endpoint::tunnel(process_resource(), invoke, tunnel->protocol_, tunnel->config_);
        }
        return route_endpoint::response_stream(invoke, body_mode);
    }

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
                            return make_endpoint(route, callback_access::invoke(view), body_mode);
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
            const auto route_limits = table->has_route_rate_limit() ? route_rate_limit_presence::present
                                                                    : route_rate_limit_presence::absent;
            auto limiter = rate_limit_ || route_limits == route_rate_limit_presence::present
                               ? make_pmr_object<rate_limiter>(process_resource(),
                                     rate_limit_ ? std::optional<rate_limit_rule>(rate_limit_->rule_) : std::nullopt, route_limits,
                                     rate_limit_ ? rate_limit_->capacity_per_worker_ : default_rate_limit_capacity_per_worker)
                               : std::unique_ptr<rate_limiter, pmr_object_deleter<rate_limiter>>{};
            if (!worker_state_definitions_.empty()) {
                worker_states_.emplace(worker_state_definitions_);
                try {
                    worker_states_->initialize();
                } catch (...) {
                    worker_states_.reset();
                    throw;
                }
            }
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
        const auto resolution = resolve_dispatched_request(table, request);
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
        services.bind_router_capabilities(self->clients_.empty() ? nullptr : &self->clients_,
            self->worker_states_ ? &*self->worker_states_ : nullptr);
        binding.bind_worker_states();
        // Only this router's own rule: the host's application-wide rule never
        // covers a request stream.
        if (const auto rejection = decide_request_rate_limit(
                self->limiter_.get(), context_value.conn().client().address(), services.pinned_arrival())) {
            auto response = co_await table.handle_error(request, memory, rate_limit_rejection_error(), services);
            apply_rate_limit_rejection_headers(response, *rejection);
            context_access::set_response(context_value, std::move(response));
            co_return;
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
        if (route.endpoint().websocket() != nullptr || route.endpoint().tunnel() != nullptr) {
            co_await dispatch_upgrade(table, stream, *resolved, memory, services);
            co_return;
        }
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
    std::optional<rate_limit_config> rate_limit_;
    borrowed_clients clients_;
    std::pmr::vector<worker_state_definition> worker_state_definitions_{process_resource()};

private:
    enum class phase { editable,
        freezing,
        frozen };

    // The route's middleware runs before its terminal accepts the upgrade
    // through the request stream, binds the session to the context and runs
    // the handler on it; the transport finishes the session afterwards.
    static task<void> run_upgrade_terminal(request_stream& stream, const resolved_route& resolved, context& context_value) {
        const auto& endpoint = resolved.route().endpoint();
        if (const auto* websocket_endpoint = endpoint.websocket()) {
            const auto accepted = co_await stream.accept_websocket_endpoint(*websocket_endpoint);
            auto* session = accepted.session();
            // A rejected handshake already selected its error response, whose
            // status the access log records when the server sends it.
            if (session == nullptr) {
                co_return;
            }
            context_websocket_binding session_binding(context_value, *session);
            co_await resolved.stream_handler()(context_value);
            co_return;
        }
        const auto status = stream.request().known_method() == http_known_method::connect ? http_status::ok
                                                                                          : http_status::switching_protocols;
        auto* tunnel = co_await stream.accept_tunnel_impl(status, endpoint.tunnel()->config());
        context_tunnel_binding tunnel_binding(context_value, *tunnel);
        co_await resolved.stream_handler()(context_value);
    }

    static task<void> dispatch_upgrade(const route_table& table, request_stream& stream, const resolved_route& resolved,
        request_memory& memory, const context_services& services) {
        auto& context_value = stream.context_;
        const auto& request = stream.request();
        const bool websocket_route = resolved.route().endpoint().websocket() != nullptr;
        // The route exists only for the upgrade its endpoint names: a plain
        // GET, or an upgrade request carrying content, cannot reach it.
        if (websocket_route ? stream.accept_websocket_ == nullptr : !stream.tunnel_requested()) {
            auto response = co_await table.handle_error(request, memory,
                websocket_route ? http_error_info({.status_ = http_status::bad_request,
                                      .code_ = "invalid_websocket_handshake",
                                      .message_ = "invalid WebSocket handshake"})
                                : http_error_info({.status_ = http_status::bad_request,
                                      .message_ = "tunnel route requires a CONNECT or upgrade request without content"}),
                services);
            context_access::set_response(context_value, std::move(response));
            co_return;
        }
        auto terminal = [&stream, &resolved](context& terminal_context) {
            return run_upgrade_terminal(stream, resolved, terminal_context);
        };
        std::exception_ptr failure;
        try {
            auto response = co_await table.dispatch_bound_stream(
                resolved, context_value, callback_access::bind<task<void>(context&)>(terminal));
            if (response) {
                context_access::set_response(context_value, std::move(*response));
            }
            co_return;
        } catch (...) {
            failure = std::current_exception();
        }
        // A failure after the upgrade head ends the established session; the
        // transport reports it.
        if (stream.response_.committed() || context_access::output_committed(context_value)) {
            std::rethrow_exception(failure);
        }
        auto response = co_await table.handle_exception(request, memory, failure, services);
        context_access::set_response(context_value, std::move(response));
    }

    phase phase_{phase::editable};
    compiled_route_plan_ptr_type plan_;
    std::unique_ptr<route_table, pmr_object_deleter<route_table>> table_;
    std::unique_ptr<rate_limiter, pmr_object_deleter<rate_limiter>> limiter_;
    std::optional<worker_state_registry> worker_states_;
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

router& router::add_websocket(std::string_view path, router_stream_handler handler, websocket_route_config config,
    std::span<const detail::controller_middleware_descriptor> modifiers) {
    state_->require_editable();
    state_->registrations_.emplace_back("GET", path, std::move(handler), modifiers, std::move(config));
    return *this;
}

router& router::add_tunnel(std::string_view protocol, std::string_view target, router_stream_handler handler,
    http_tunnel_route_config config, std::span<const detail::controller_middleware_descriptor> modifiers) {
    state_->require_editable();
    state_->registrations_.emplace_back("CONNECT", target, std::move(handler), modifiers,
        detail::router_state::tunnel_upgrade{std::pmr::string(protocol, process_resource()), config});
    return *this;
}

router& router::add_worker_state(detail::worker_state_definition definition) {
    state_->require_editable();
    detail::append_worker_state_definition(state_->worker_state_definitions_, std::move(definition));
    return *this;
}

router& router::rate_limit(rate_limit_config config) & {
    state_->require_editable();
    detail::validate_rate_limit_config(config);
    state_->rate_limit_ = config;
    return *this;
}

#ifdef RUVIA_ENABLE_REDIS
router& router::bind_redis(std::string_view alias, const redis_client& client) & {
    state_->require_editable();
    state_->clients_.bind_redis(state_->worker_, alias, client);
    return *this;
}
#endif

#ifdef RUVIA_ENABLE_DATABASE
router& router::bind_database(std::string_view alias, const db_client& client) & {
    state_->require_editable();
    state_->clients_.bind_database(state_->worker_, alias, client);
    return *this;
}
#endif

router& router::bind_http_client(const http_client& clients) & {
    state_->require_editable();
    state_->clients_.bind_http_client(state_->worker_, clients);
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
