#include <cstdint>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "ruvia/core/memory/pmr_resource.h"
#include "ruvia/http/http_known_method.h"
#include "ruvia/http/url_encoding.h"
#include "ruvia/web/detail/router/prefix_fallback.h"

#include "router/path_segments.h"
#include "router/router_impl.h"

namespace ruvia {
namespace {

[[nodiscard]] bool websocket_subprotocols_equal(const std::pmr::vector<std::pmr::string>& owned,
    std::span<const std::string_view> borrowed) noexcept {
    if (owned.size() != borrowed.size()) {
        return false;
    }
    for (std::size_t i = 0; i < owned.size(); ++i) {
        if (owned[i] != borrowed[i]) {
            return false;
        }
    }
    return true;
}

// How much of a route's request-path space one path-scoped middleware covers.
enum class middleware_scope_coverage : std::uint8_t {
    // No request the route can match is under the scope: the frame is omitted.
    none,
    // Some matched requests are under the scope and some are not: the frame is
    // kept with its scope and checked against the request path at dispatch.
    partial,
    // Every request the route can match is under the scope: the frame is
    // unconditional.
    full,
};

// Classifies a route pattern against a normalized scope prefix with the
// matcher's own segment rules, so the answer holds for the request paths the
// route actually serves rather than for the pattern text. A request is under
// the scope exactly when its leading segments are equivalent to the prefix's
// segments (path_is_under_prefix); a literal route segment matches only its own
// raw spelling, so comparing it with the same equivalence is exact.
[[nodiscard]] middleware_scope_coverage route_scope_coverage(
    std::string_view pattern, bool dynamic, std::string_view prefix) noexcept {
    if (!dynamic) {
        // A static route serves exactly its own path.
        return detail::path_is_under_prefix(pattern, prefix) ? middleware_scope_coverage::full
                                                             : middleware_scope_coverage::none;
    }
    if (prefix.empty() || prefix == "/") {
        return middleware_scope_coverage::full;
    }

    // Set once a parameter faces a prefix segment: it may or may not capture
    // that exact text, so only the request can decide.
    bool request_dependent = false;
    for (;;) {
        std::string_view prefix_segment;
        std::string_view prefix_rest;
        if (!detail::split_path_segment(prefix, prefix_segment, prefix_rest)) {
            return request_dependent ? middleware_scope_coverage::partial : middleware_scope_coverage::full;
        }
        std::string_view pattern_segment;
        std::string_view pattern_rest;
        if (!detail::split_path_segment(pattern, pattern_segment, pattern_rest)) {
            // Every matched request ends above the scope.
            return middleware_scope_coverage::none;
        }
        if (pattern_segment == "*") {
            // The capture may be empty or may spell out the rest of the prefix.
            return middleware_scope_coverage::partial;
        }
        if (!pattern_segment.empty() && pattern_segment.front() == ':') {
            if (prefix_segment.empty()) {
                // A parameter never matches an empty segment.
                return middleware_scope_coverage::none;
            }
            request_dependent = true;
        } else if (!url_components_equivalent(pattern_segment, prefix_segment, url_decode_mode::percent)) {
            return middleware_scope_coverage::none;
        }
        prefix = prefix_rest;
        pattern = pattern_rest;
    }
}

[[nodiscard]] std::string describe_route(http_known_method method, std::string_view path) {
    std::string text(known_http_method_token(method));
    text.push_back(' ');
    text.append(path);
    return text;
}

}  // namespace

detail::route_entry::route_entry(std::pmr::memory_resource* resource, init_type init)
    : route_entry(detail::resolved_pmr_resource_tag{}, detail::pmr_resource_or_default(resource),
          std::move(init)) {}

detail::route_entry::route_entry(
    detail::resolved_pmr_resource_tag, std::pmr::memory_resource* resource, init_type init)
    : method_(init.method_),
      method_token_(init.method_token_, resource),
      path_(init.path_, resource),
      endpoint_(std::move(init.endpoint_)),
      dynamic_(init.dynamic_),
      max_request_body_bytes_(init.max_request_body_bytes_),
      deadline_ms_(init.deadline_ms_),
      middleware_offset_(init.middleware_offset_),
      middleware_count_(init.middleware_count_) {}

detail::route_table::route_table(std::pmr::memory_resource* resource)
    : resource_(detail::pmr_resource_or_default(resource)),
      routes_(resource_),
      middleware_frames_(resource_),
      middleware_scopes_(resource_),
      server_extension_method_tokens_(resource_),
      dynamic_param_names_(resource_),
      owned_plan_(nullptr, pmr_object_deleter<compiled_route_plan>{resource_}) {
    owned_plan_ = make_pmr_object<compiled_route_plan>(resource_, resource_);
    plan_ = owned_plan_.get();
}

detail::compiled_route_plan_ptr_type detail::route_table::release_compiled_plan() {
    if (owned_plan_ == nullptr || plan_ != owned_plan_.get()) {
        throw std::logic_error("route table does not own a compiled plan");
    }
    auto result_value = std::move(owned_plan_);
    plan_ = result_value.get();
    return result_value;
}

void detail::route_table::capture_route_identities() {
    auto& identities = owned_plan_->identities_;
    identities.reserve(routes_.size());
    for (const auto& route : routes_) {
        auto& identity = identities.emplace_back(owned_plan_->resource_);
        identity.method_ = route.method();
        identity.method_token_ = route.method_token();
        identity.path_ = route.path();
        identity.dynamic_ = route.dynamic();
        identity.max_request_body_bytes_ = route.max_request_body_bytes();
        identity.deadline_ms_ = route.deadline_ms();

        const auto& endpoint = route.endpoint();
        if (const auto* buffered = endpoint.buffered()) {
            identity.endpoint_kind_ = compiled_route_plan::endpoint_kind_type::buffered;
            identity.request_body_mode_ = buffered->request_body_mode();
            identity.replay_safe_ = buffered->replay_safe();
            identity.buffered_invoke_ = buffered->handler().invoke();
        } else if (const auto* stream = endpoint.response_stream()) {
            identity.endpoint_kind_ = compiled_route_plan::endpoint_kind_type::response_stream;
            identity.response_stream_kind_ = stream->kind();
            identity.stream_invoke_ = stream->handler().invoke();
        } else if (const auto* tunnel = endpoint.tunnel()) {
            identity.endpoint_kind_ = compiled_route_plan::endpoint_kind_type::tunnel;
            identity.stream_invoke_ = tunnel->handler().invoke();
            identity.tunnel_protocol_ = tunnel->protocol();
            identity.tunnel_peer_transport_fin_timeout_ms_ = tunnel->config().peer_transport_fin_timeout_.count();
            identity.tunnel_datagrams_ = tunnel->config().datagrams_;
        } else {
            const auto& websocket_value = *endpoint.get_websocket();
            identity.endpoint_kind_ = compiled_route_plan::endpoint_kind_type::websocket;
            identity.stream_invoke_ = websocket_value.handler().invoke();
            identity.websocket_deflate_enabled_ = websocket_value.deflate().enabled_;
            identity.websocket_compression_level_ = websocket_value.deflate().compression_level_;
            identity.websocket_context_takeover_ = websocket_value.deflate().context_takeover_;
            identity.websocket_subprotocols_.reserve(websocket_value.subprotocols().size());
            for (const auto subprotocol : websocket_value.subprotocols()) {
                identity.websocket_subprotocols_.emplace_back(subprotocol);
            }
            if (websocket_value.lifecycle().heartbeat_.ping_interval_.has_value()) {
                identity.websocket_ping_interval_ms_ =
                    websocket_value.lifecycle().heartbeat_.ping_interval_->count();
                identity.websocket_pong_timeout_ms_ =
                    websocket_value.lifecycle().heartbeat_.pong_timeout_->count();
            }
            if (websocket_value.lifecycle().close_handshake_timeout_.has_value()) {
                identity.websocket_close_timeout_ms_ =
                    websocket_value.lifecycle().close_handshake_timeout_->count();
            }
            identity.websocket_peer_transport_fin_timeout_ms_ =
                websocket_value.lifecycle().peer_transport_fin_timeout_.count();
        }

        identity.middleware_invokes_.reserve(route.middleware_count());
        identity.middleware_scopes_.reserve(route.middleware_count());
        for (std::size_t i = 0; i < route.middleware_count(); ++i) {
            identity.middleware_invokes_.push_back(
                middleware_frames_[route.middleware_offset() + i].invoke());
            identity.middleware_scopes_.push_back(middleware_scopes_[route.middleware_offset() + i]);
        }
    }

    auto& unmatched_invokes = owned_plan_->unmatched_middleware_invokes_;
    unmatched_invokes.reserve(unmatched_middleware_count_);
    for (std::size_t i = 0; i < unmatched_middleware_count_; ++i) {
        unmatched_invokes.push_back(middleware_frames_[unmatched_middleware_offset_ + i].invoke());
    }
    owned_plan_->has_route_rate_limit_ = has_route_rate_limit_;
}

void detail::route_table::bind_compiled_plan(const compiled_route_plan& plan) {
    if (plan.identities_.size() != routes_.size() ||
        plan.has_route_rate_limit_ != has_route_rate_limit_ ||
        plan.unmatched_middleware_invokes_.size() != unmatched_middleware_count_) {
        throw std::logic_error("worker route table differs from the compiled application plan");
    }

    for (std::size_t i = 0; i < unmatched_middleware_count_; ++i) {
        if (middleware_frames_[unmatched_middleware_offset_ + i].invoke() !=
            plan.unmatched_middleware_invokes_[i]) {
            throw std::logic_error("worker route table differs from the compiled application plan");
        }
    }

    for (std::size_t i = 0; i < routes_.size(); ++i) {
        const auto& route = routes_[i];
        const auto& identity = plan.identities_[i];
        if (route.method() != identity.method_ || route.method_token() != identity.method_token_ ||
            route.path() != identity.path_ || route.dynamic() != identity.dynamic_ ||
            route.max_request_body_bytes() != identity.max_request_body_bytes_ ||
            route.deadline_ms() != identity.deadline_ms_ ||
            (route.endpoint().buffered() != nullptr &&
                route.endpoint().buffered()->replay_safe() != identity.replay_safe_) ||
            route.middleware_count() != identity.middleware_invokes_.size()) {
            throw std::logic_error("worker route table differs from the compiled application plan");
        }

        for (std::size_t middleware_value = 0; middleware_value < route.middleware_count(); ++middleware_value) {
            if (middleware_frames_[route.middleware_offset() + middleware_value].invoke() !=
                    identity.middleware_invokes_[middleware_value] ||
                middleware_scopes_[route.middleware_offset() + middleware_value] !=
                    identity.middleware_scopes_[middleware_value]) {
                throw std::logic_error(
                    "worker route table differs from the compiled application plan");
            }
        }

        const auto& endpoint = route.endpoint();
        bool endpoint_matches = false;
        if (const auto* buffered = endpoint.buffered()) {
            endpoint_matches = identity.endpoint_kind_ == compiled_route_plan::endpoint_kind_type::buffered &&
                               identity.request_body_mode_ == buffered->request_body_mode() &&
                               identity.replay_safe_ == buffered->replay_safe() &&
                               identity.buffered_invoke_ == buffered->handler().invoke();
        } else if (const auto* stream = endpoint.response_stream()) {
            endpoint_matches =
                identity.endpoint_kind_ == compiled_route_plan::endpoint_kind_type::response_stream &&
                identity.response_stream_kind_ == stream->kind() &&
                identity.stream_invoke_ == stream->handler().invoke();
        } else if (const auto* tunnel = endpoint.tunnel()) {
            endpoint_matches = identity.endpoint_kind_ == compiled_route_plan::endpoint_kind_type::tunnel &&
                               identity.stream_invoke_ == tunnel->handler().invoke() && identity.tunnel_protocol_ == tunnel->protocol() &&
                               identity.tunnel_peer_transport_fin_timeout_ms_ == tunnel->config().peer_transport_fin_timeout_.count() &&
                               identity.tunnel_datagrams_ == tunnel->config().datagrams_;
        } else {
            const auto& websocket_value = *endpoint.get_websocket();
            const auto ping_interval_ms = websocket_value.lifecycle().heartbeat_.ping_interval_.has_value()
                                              ? websocket_value.lifecycle().heartbeat_.ping_interval_->count()
                                              : std::int64_t{-1};
            const auto pong_timeout_ms = websocket_value.lifecycle().heartbeat_.ping_interval_.has_value()
                                             ? websocket_value.lifecycle().heartbeat_.pong_timeout_->count()
                                             : std::int64_t{-1};
            const auto close_timeout_ms = websocket_value.lifecycle().close_handshake_timeout_.has_value()
                                              ? websocket_value.lifecycle().close_handshake_timeout_->count()
                                              : std::int64_t{-1};
            const auto peer_transport_fin_timeout_ms =
                websocket_value.lifecycle().peer_transport_fin_timeout_.count();
            endpoint_matches =
                identity.endpoint_kind_ == compiled_route_plan::endpoint_kind_type::websocket &&
                identity.stream_invoke_ == websocket_value.handler().invoke() &&
                websocket_subprotocols_equal(
                    identity.websocket_subprotocols_, websocket_value.subprotocols()) &&
                identity.websocket_ping_interval_ms_ == ping_interval_ms &&
                identity.websocket_pong_timeout_ms_ == pong_timeout_ms &&
                identity.websocket_close_timeout_ms_ == close_timeout_ms &&
                identity.websocket_peer_transport_fin_timeout_ms_ == peer_transport_fin_timeout_ms &&
                identity.websocket_deflate_enabled_ == websocket_value.deflate().enabled_ &&
                identity.websocket_compression_level_ == websocket_value.deflate().compression_level_ &&
                identity.websocket_context_takeover_ == websocket_value.deflate().context_takeover_;
        }
        if (!endpoint_matches) {
            throw std::logic_error("worker route table differs from the compiled application plan");
        }
    }
    owned_plan_.reset();
    plan_ = &plan;
    build_server_extension_method_tokens();
    bind_dynamic_param_names();
}

void detail::route_table::build_server_extension_method_tokens() {
    server_extension_method_tokens_.clear();
    server_extension_method_tokens_.reserve(plan_->extension_route_indices_.size());
    for (const auto route_index : plan_->extension_route_indices_) {
        const auto token = routes_[route_index].method_token();
        if (std::ranges::find(server_extension_method_tokens_, token) == server_extension_method_tokens_.end()) {
            server_extension_method_tokens_.push_back(token);
        }
    }
}

void detail::router_impl::validate_no_dynamic_route_conflict(std::span<const pending_route_type> routes_value) {
    for (std::size_t i = 0; i < routes_value.size(); ++i) {
        const auto& left = routes_value[i];
        if (!left.dynamic()) {
            continue;
        }
        const auto* left_tunnel = left.endpoint().tunnel();
        for (std::size_t j = i + 1; j < routes_value.size(); ++j) {
            const auto& right = routes_value[j];
            if (!right.dynamic() || left.method() != right.method()) {
                continue;
            }
            // Extended CONNECT routes share one lookup trie per protocol; every
            // other dynamic route shares one per method. Routes in different
            // tries never compete for a request.
            const auto* right_tunnel = right.endpoint().tunnel();
            if ((left_tunnel == nullptr) != (right_tunnel == nullptr) ||
                (left_tunnel != nullptr && left_tunnel->protocol() != right_tunnel->protocol())) {
                continue;
            }
            if (route_table::same_dynamic_shape(left.path(), right.path())) {
                throw std::invalid_argument("conflicting dynamic routes match the same requests with equal precedence: " +
                                            describe_route(left.method(), left.path()) + " and " +
                                            describe_route(right.method(), right.path()));
            }
        }
    }
}

void detail::router_impl::build_route_table(
    detail::route_table& table_value, const compiled_route_plan* compiled_plan) const {
    std::size_t middleware_count = 0;
    for (const auto& route : pending_routes_) {
        middleware_count += global_middleware_frames_.size() + route.middlewares().size();
    }
    middleware_count += global_middleware_frames_.size();
    table_value.routes_.reserve(pending_routes_.size());
    table_value.middleware_frames_.reserve(middleware_count);
    table_value.middleware_scopes_.reserve(middleware_count);

    for (const auto& pending : pending_routes_) {
        const auto pending_middlewares = pending.middlewares();
        route_entry route(detail::resolved_pmr_resource_tag{}, table_value.resource_,
            route_entry::init_type{.method_ = pending.method(),
                .method_token_ = pending.method_token(),
                .path_ = pending.path(),
                .endpoint_ = pending.endpoint().clone(table_value.resource_),
                .dynamic_ = pending.dynamic(),
                .max_request_body_bytes_ = pending.max_request_body_bytes(),
                .deadline_ms_ = pending.deadline_ms(),
                .middleware_offset_ = 0,
                .middleware_count_ = 0});
        // application-wide middleware runs before controller/route middleware on every
        // matched route: each route's contiguous frame range starts with the
        // shared global instances.
        //
        // A path-scoped registration (use_at) applies to the request paths
        // under its prefix, decided here once per route against every path the
        // route can match: a route entirely inside the scope gets an
        // unconditional frame, a route entirely outside gets none, and only a
        // route that serves paths on both sides (a parameter or wildcard facing
        // the prefix) keeps the frame with its scope for a dispatch-time check.
        // middleware_scopes_ runs parallel to middleware_frames_; an empty scope
        // is unconditional. global_middleware_frames_ and
        // global_middleware_descriptors_ are parallel, so the descriptor at i
        // carries frame i's scope.
        const auto middleware_offset = table_value.middleware_frames_.size();
        bool conditional_middleware = false;
        for (std::size_t i = 0; i < global_middleware_frames_.size(); ++i) {
            const auto scope = global_middleware_descriptors_[i].prefix();
            const auto coverage = route_scope_coverage(pending.path(), pending.dynamic(), scope);
            if (coverage == middleware_scope_coverage::none) {
                continue;
            }
            const auto conditional = coverage == middleware_scope_coverage::partial;
            conditional_middleware = conditional_middleware || conditional;
            table_value.middleware_frames_.push_back(global_middleware_frames_[i]);
            table_value.middleware_scopes_.push_back(conditional ? scope : std::string_view{});
        }
        table_value.middleware_frames_.insert(
            table_value.middleware_frames_.end(), pending_middlewares.begin(), pending_middlewares.end());
        table_value.middleware_scopes_.resize(table_value.middleware_frames_.size());
        route.set_middleware_range(middleware_offset,
            table_value.middleware_frames_.size() - middleware_offset, conditional_middleware);
        table_value.routes_.push_back(std::move(route));
    }

    // The unmatched-request block, appended once after every route's range so
    // it stays contiguous and no route can accidentally include it.
    table_value.unmatched_middleware_offset_ = table_value.middleware_frames_.size();
    for (std::size_t i = 0; i < global_middleware_frames_.size(); ++i) {
        if (global_middleware_descriptors_[i].runs_on_unmatched_requests()) {
            table_value.middleware_frames_.push_back(global_middleware_frames_[i]);
            table_value.middleware_scopes_.emplace_back();
        }
    }
    table_value.unmatched_middleware_count_ =
        table_value.middleware_frames_.size() - table_value.unmatched_middleware_offset_;

    if (compiled_plan != nullptr) {
        table_value.bind_compiled_plan(*compiled_plan);
        return;
    }

    for (std::size_t i = 0; i < table_value.routes_.size(); ++i) {
        if (table_value.routes_[i].endpoint().tunnel() != nullptr) {
            table_value.owned_plan_->connect_route_indices_.push_back(i);
        }
        if (table_value.routes_[i].method() == http_known_method::unknown) {
            table_value.owned_plan_->extension_route_indices_.push_back(i);
        }
    }
    table_value.capture_route_identities();
    table_value.build_allowed_method_mask();
    table_value.build_static_index();
    table_value.build_dynamic_routes();
}

}  // namespace ruvia
