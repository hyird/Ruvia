#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory_resource>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "ruvia/core/stop_token.h"
#include "ruvia/core/worker_handle.h"
#include "ruvia/http/http_limits.h"
#include "ruvia/http/http_priority.h"
#include "ruvia/web/conn_info.h"
#include "ruvia/web/error_handlers.h"
#include "ruvia/web/http3_early_data_info.h"

#include "context/context_capabilities.h"
#include "integration/worker_client_registry_view.h"
#include "server/trusted_proxies.h"

namespace ruvia {

class http_request;
class http_request_trailers;
class blocking_pool;
class env;
}  // namespace ruvia

namespace ruvia::detail {

class rate_limiter;
class http_interim_response_output;
class http_connection_advertisement_output;
class http_push_output;
class route_table;
class worker_state_registry;

struct context_worker_services final {
    std::reference_wrapper<const worker_handle> worker_;
    worker_client_registry_view clients_{worker_client_registry_view::detached()};
    rate_limiter* rate_limiter_{};
    const ruvia::env* env_{};
    std::size_t max_decoded_body_bytes_{default_max_buffered_body_bytes};
    std::pmr::memory_resource* inbound_buffer_pool_{};
    const route_table* routes_{};
    const worker_state_registry* states_{};
    blocking_pool* blocking_pool_{};
    bool precompressed_static_files_{};
    const trusted_proxy_set* trusted_proxies_{};
};

struct context_connection_services final {
    std::string_view automatic_alt_svc_{};
    conn_info info_;
    // Whether the peer is a configured trusted proxy. Decided once per
    // connection: the peer address never changes, so its IP parse and block
    // match are not repeated per request.
    bool peer_trusted_{false};
    http3_early_data_info early_data_{};
};

// The per-request protocol channels a driver binds once, before admission. A
// null member is a channel this protocol, or this request, does not provide.
// Each borrowed object outlives every context created for the request.
struct context_request_channels final {
    http_interim_response_output* interim_output_{};
    http_connection_advertisement_output* connection_advertisements_{};
    http_push_output* push_output_{};
    const std::optional<http_priority>* priority_update_{};
};

struct context_request_services final {
    std::reference_wrapper<const stop_token> stop_token_;
    context_request_body_source body_source_{};
    const http_request_trailers* trailers_{};
    // The driver's 1xx output, bound once before admission; null when this
    // request has none. It outlives every context created for the request.
    context_request_channels channels_{};
    context_response_output response_output_{};
    std::size_t dispatch_depth_{};
    // The matched route's declared body_limit; 0 when it declares none.
    std::size_t route_body_limit_{};
};

class context_services final {
public:
    context_services() = delete;

    context_services(context_worker_services worker_services, const stop_token& stop_token_value)
        : worker_services_(worker_services),
          request_services_{.stop_token_ = stop_token_value} {
        if (!worker_services_.worker_.get().valid()) {
            throw std::invalid_argument("context services require a valid worker");
        }
    }
    context_services(context_worker_services, stop_token&&) = delete;

    [[nodiscard]] constexpr worker_client_registry_view client_registries() const noexcept {
        return worker_services_.clients_;
    }

    [[nodiscard]] detail::rate_limiter* rate_limiter() const noexcept {
        return worker_services_.rate_limiter_;
    }

    [[nodiscard]] const ruvia::env* env() const noexcept {
        return worker_services_.env_;
    }

    [[nodiscard]] std::pmr::memory_resource* inbound_buffer_pool() const noexcept {
        return worker_services_.inbound_buffer_pool_;
    }

    [[nodiscard]] context_services with_inbound_buffer_pool(std::pmr::memory_resource& resource) const noexcept {
        auto services = *this;
        services.worker_services_.inbound_buffer_pool_ = &resource;
        return services;
    }
    context_services with_inbound_buffer_pool(std::pmr::memory_resource&&) const = delete;

    [[nodiscard]] constexpr std::size_t max_decoded_body_bytes() const noexcept {
        return worker_services_.max_decoded_body_bytes_;
    }

    // Ceiling for this request's decoded Content-Encoding representation. A
    // route's body_limit bounds what the route reads, not only the wire
    // bytes: a small compressed body must not inflate past it up to the
    // worker-wide bound.
    [[nodiscard]] constexpr std::size_t max_decoded_request_body_bytes() const noexcept {
        const auto route_limit = request_services_.route_body_limit_;
        return route_limit != 0 && route_limit < worker_services_.max_decoded_body_bytes_
                   ? route_limit
                   : worker_services_.max_decoded_body_bytes_;
    }

    [[nodiscard]] context_services with_route_body_limit(std::size_t limit) const noexcept {
        auto services = *this;
        services.request_services_.route_body_limit_ = limit;
        return services;
    }

    [[nodiscard]] const worker_handle& worker() const noexcept {
        return worker_services_.worker_.get();
    }

    [[nodiscard]] const stop_token& get_stop_token() const noexcept {
        return request_services_.stop_token_.get();
    }

    [[nodiscard]] const route_table* routes() const noexcept {
        return worker_services_.routes_;
    }

    // Every dispatch path hands the context the table it may re-enter, which
    // also selects the not-found handler for the request path when the
    // handler asks for one. The route table is server-owned and outlives every
    // dispatched request.
    [[nodiscard]] context_services with_routes(const route_table& routes_value) const noexcept {
        auto services = *this;
        services.worker_services_.routes_ = &routes_value;
        return services;
    }
    context_services with_routes(route_table&&) const = delete;

    [[nodiscard]] constexpr const context_request_body_source& request_body_source() const noexcept {
        return request_services_.body_source_;
    }

    [[nodiscard]] context_services with_streaming_request_body(body_reader& value) const noexcept {
        auto services = *this;
        services.request_services_.body_source_ = context_request_body_source::streaming(value);
        return services;
    }

    [[nodiscard]] context_services with_lazy_request_body(request_body_loader& value) const noexcept {
        auto services = *this;
        services.request_services_.body_source_ = context_request_body_source::lazy(value);
        return services;
    }

    [[nodiscard]] const http_request_trailers* request_trailers() const noexcept {
        return request_services_.trailers_;
    }

    // Bound by whichever layer owns the request body, alongside its source.
    [[nodiscard]] context_services with_request_trailers(const http_request_trailers& trailers) const noexcept {
        auto services = *this;
        services.request_services_.trailers_ = &trailers;
        return services;
    }
    context_services with_request_trailers(http_request_trailers&&) const = delete;

    [[nodiscard]] http_interim_response_output* interim_output() const noexcept {
        return request_services_.channels_.interim_output_;
    }

    [[nodiscard]] http_connection_advertisement_output* connection_advertisements() const noexcept {
        return request_services_.channels_.connection_advertisements_;
    }

    [[nodiscard]] http_push_output* push_output() const noexcept {
        return request_services_.channels_.push_output_;
    }

    [[nodiscard]] const std::optional<http_priority>* request_priority_update() const noexcept {
        return request_services_.channels_.priority_update_;
    }

    [[nodiscard]] context_services with_request_channels(const context_request_channels& channels) const noexcept {
        auto services = *this;
        services.request_services_.channels_ = channels;
        return services;
    }

    [[nodiscard]] context_services with_interim_output(http_interim_response_output& output) const noexcept {
        auto channels = request_services_.channels_;
        channels.interim_output_ = &output;
        return with_request_channels(channels);
    }

    [[nodiscard]] constexpr context_response_output& response_output() noexcept {
        return request_services_.response_output_;
    }

    [[nodiscard]] constexpr const context_response_output& response_output() const noexcept {
        return request_services_.response_output_;
    }

    [[nodiscard]] context_services with_response_stream(response_stream_writer& value) const noexcept {
        auto services = *this;
        services.request_services_.response_output_ = context_response_output::response_stream(value);
        return services;
    }

    [[nodiscard]] std::size_t dispatch_depth() const noexcept {
        return request_services_.dispatch_depth_;
    }

    // Subrequests inherit the parent connection identity, one dispatch level deeper.
    [[nodiscard]] context_services for_subrequest(const stop_token& stop_token_value) const {
        context_services services(worker_services_, stop_token_value);
        services.connection_services_.info_ = connection_services_.info_;
        services.request_services_.dispatch_depth_ = request_services_.dispatch_depth_ + 1;
        services.connection_services_.early_data_ = connection_services_.early_data_;
        return services;
    }

    [[nodiscard]] blocking_pool* get_blocking_pool() const noexcept {
        return worker_services_.blocking_pool_;
    }

    // Server-owned static-file policy. When response compression is enabled,
    // static_file() may negotiate an indexed precompressed variant. It never
    // performs request-time file compression.
    [[nodiscard]] constexpr bool precompressed_static_files() const noexcept {
        return worker_services_.precompressed_static_files_;
    }

    [[nodiscard]] const worker_state_registry* worker_states() const noexcept {
        return worker_services_.states_;
    }

    [[nodiscard]] constexpr std::string_view automatic_alt_svc() const noexcept {
        return connection_services_.automatic_alt_svc_;
    }

    // The listener owns this value until every request context has retired.
    // An application header with the same name replaces this default.
    [[nodiscard]] context_services with_automatic_alt_svc(std::string_view value) const noexcept {
        auto services = *this;
        services.connection_services_.automatic_alt_svc_ = value;
        return services;
    }

    template <typename traits_type, typename allocator_type>
    context_services with_automatic_alt_svc(
        std::basic_string<char, traits_type, allocator_type>&&) const = delete;

    [[nodiscard]] constexpr const conn_info& get_conn_info() const noexcept {
        return connection_services_.info_;
    }

    [[nodiscard]] constexpr http3_early_data_info early_data_info() const noexcept {
        return connection_services_.early_data_;
    }

    [[nodiscard]] context_services with_early_data_info(
        http3_early_data_info value) const noexcept {
        auto services = *this;
        services.connection_services_.early_data_ = value;
        return services;
    }

    // The connection metadata one request sees. Identical to get_conn_info() unless
    // the peer is a configured trusted proxy, in which case its forwarding
    // headers name the client. Resolved per request rather than per connection:
    // each keep-alive request may carry different forwarding headers. Admission
    // resolves it once and binds the result with with_conn_info, so every
    // context created for the request reads get_conn_info().
    [[nodiscard]] conn_info resolve_conn_info(const http_request& request) const noexcept;

    [[nodiscard]] context_services with_conn_info(const conn_info& resolved) const noexcept {
        auto services = *this;
        services.connection_services_.info_ = resolved;
        return services;
    }

    // Views borrow connection-owned storage and remain valid for every context
    // created while that connection is dispatched.
    [[nodiscard]] context_services with_plain_transport(
        std::string_view remote_address, std::uint16_t remote_port) const noexcept {
        auto services = *this;
        services.connection_services_.info_ = conn_info::plain(remote_address);
        services.connection_services_.info_.set_remote_port(remote_port);
        services.connection_services_.peer_trusted_ = trusts_peer(remote_address);
        return services;
    }

    template <typename traits_type, typename allocator_type>
    context_services with_plain_transport(
        std::basic_string<char, traits_type, allocator_type>&&, std::uint16_t) const = delete;

    [[nodiscard]] context_services with_tls_transport(std::string_view remote_address,
        std::string_view client_certificate_subject, std::uint16_t remote_port) const noexcept {
        auto services = *this;
        services.connection_services_.info_ = conn_info::tls(remote_address, client_certificate_subject);
        services.connection_services_.info_.set_remote_port(remote_port);
        services.connection_services_.peer_trusted_ = trusts_peer(remote_address);
        return services;
    }

    template <typename traits_type, typename allocator_type>
    context_services with_tls_transport(
        std::basic_string<char, traits_type, allocator_type>&&, std::string_view, std::uint16_t) const = delete;

    template <typename traits_type, typename allocator_type>
    context_services with_tls_transport(
        std::string_view, std::basic_string<char, traits_type, allocator_type>&&, std::uint16_t) const = delete;

private:
    [[nodiscard]] bool trusts_peer(std::string_view remote_address) const noexcept {
        return worker_services_.trusted_proxies_ != nullptr && worker_services_.trusted_proxies_->trusts(remote_address);
    }

    context_worker_services worker_services_;
    context_connection_services connection_services_{.info_ = conn_info::plain({})};
    context_request_services request_services_;
};

}  // namespace ruvia::detail
