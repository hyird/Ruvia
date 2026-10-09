#include <memory>
#include <span>
#include <stdexcept>
#include <string_view>
#include <utility>

#include "ruvia/core/memory/pmr_resource.h"
#include "ruvia/web/detail/router/prefix_fallback.h"

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
        for (std::size_t i = 0; i < route.middleware_count(); ++i) {
            identity.middleware_invokes_.push_back(
                middleware_frames_[route.middleware_offset() + i].invoke());
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
                identity.middleware_invokes_[middleware_value]) {
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
        for (std::size_t j = i + 1; j < routes_value.size(); ++j) {
            const auto& right = routes_value[j];
            if (!right.dynamic() || left.method() != right.method() ||
                (left.endpoint().tunnel() != nullptr && right.endpoint().tunnel() != nullptr &&
                    left.endpoint().tunnel()->protocol() != right.endpoint().tunnel()->protocol())) {
                continue;
            }
            if (route_table::same_dynamic_shape(left.path(), right.path())) {
                throw std::invalid_argument("conflicting dynamic route shape");
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
    table_value.routes_.reserve(pending_routes_.size());
    table_value.middleware_frames_.reserve(middleware_count);

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
        // A path-scoped registration (use_at) is filtered out HERE, when the
        // table is built, rather than tested per request: a route outside the
        // scope simply never receives the frame, so scoping costs the request
        // path nothing. global_middleware_frames_ and global_middleware_descriptors_
        // are parallel, so the descriptor at i carries frame i's scope.
        const auto middleware_offset = table_value.middleware_frames_.size();
        for (std::size_t i = 0; i < global_middleware_frames_.size(); ++i) {
            if (!path_is_under_prefix(pending.path(), global_middleware_descriptors_[i].prefix())) {
                continue;
            }
            table_value.middleware_frames_.push_back(global_middleware_frames_[i]);
        }
        table_value.middleware_frames_.insert(
            table_value.middleware_frames_.end(), pending_middlewares.begin(), pending_middlewares.end());
        route.set_middleware_range(
            middleware_offset, table_value.middleware_frames_.size() - middleware_offset);
        table_value.routes_.push_back(std::move(route));
    }

    // The unmatched-request block, appended once after every route's range so
    // it stays contiguous and no route can accidentally include it.
    table_value.unmatched_middleware_offset_ = table_value.middleware_frames_.size();
    for (std::size_t i = 0; i < global_middleware_frames_.size(); ++i) {
        if (global_middleware_descriptors_[i].runs_on_unmatched_requests()) {
            table_value.middleware_frames_.push_back(global_middleware_frames_[i]);
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
