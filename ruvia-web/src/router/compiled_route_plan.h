#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <memory_resource>
#include <string>
#include <string_view>
#include <vector>

#include "ruvia/core/memory/pmr_object.h"
#include "ruvia/http/http_known_method.h"
#include "ruvia/http/http_response_stream.h"

#include "router/route_endpoint.h"

namespace ruvia::detail {

class route_table;
class router_impl;

// Process-level immutable lookup metadata. Route handlers and middleware
// instances remain in each worker's route_table; this plan stores only stable
// route indices and owned path structure, so every worker can share it safely.
class compiled_route_plan final {
public:
    explicit compiled_route_plan(std::pmr::memory_resource* resource)
        : resource_(pmr_resource_or_default(resource)),
          identities_(resource_),
          extension_route_indices_(resource_),
          connect_route_indices_(resource_),
          connect_protocols_(resource_),
          static_slots_(resource_),
          dynamic_roots_{dynamic_node_type(resource_), dynamic_node_type(resource_), dynamic_node_type(resource_),
              dynamic_node_type(resource_), dynamic_node_type(resource_), dynamic_node_type(resource_),
              dynamic_node_type(resource_)},
          dynamic_node_arena_(resource_),
          unmatched_middleware_invokes_(resource_) {}

    compiled_route_plan(const compiled_route_plan&) = delete;
    compiled_route_plan& operator=(const compiled_route_plan&) = delete;
    compiled_route_plan(compiled_route_plan&&) = delete;
    compiled_route_plan& operator=(compiled_route_plan&&) = delete;

private:
    friend class route_table;
    friend class router_impl;

    static constexpr std::size_t routable_method_count = 7;
    static constexpr std::size_t no_route_index = std::numeric_limits<std::size_t>::max();

    enum class endpoint_kind_type : std::uint8_t {
        buffered,
        response_stream,
        websocket,
        tunnel,
    };

    struct route_identity_type final {
        explicit route_identity_type(std::pmr::memory_resource* resource)
            : method_token_(resource),
              path_(resource),
              tunnel_protocol_(resource),
              websocket_subprotocols_(resource),
              middleware_invokes_(resource) {}

        http_known_method method_{http_known_method::unknown};
        std::pmr::string method_token_;
        std::pmr::string path_;
        std::pmr::string tunnel_protocol_;
        std::int64_t tunnel_peer_transport_fin_timeout_ms_{5000};
        bool tunnel_datagrams_{};
        bool dynamic_{false};
        endpoint_kind_type endpoint_kind_{endpoint_kind_type::buffered};
        request_body_mode request_body_mode_{request_body_mode::buffered};
        bool replay_safe_{};
        http_response_stream_kind response_stream_kind_{http_response_stream_kind::generic};
        route_handler_type::invoke_type buffered_invoke_{nullptr};
        route_stream_handler_type::invoke_type stream_invoke_{nullptr};
        std::pmr::vector<std::pmr::string> websocket_subprotocols_;
        std::int64_t websocket_ping_interval_ms_{-1};
        std::int64_t websocket_pong_timeout_ms_{-1};
        std::int64_t websocket_close_timeout_ms_{-1};
        std::int64_t websocket_peer_transport_fin_timeout_ms_{5000};
        bool websocket_deflate_enabled_{true};
        int websocket_compression_level_{6};
        bool websocket_context_takeover_{false};
        std::size_t max_request_body_bytes_{0};
        std::int64_t deadline_ms_{0};
        std::pmr::vector<route_middleware_type::invoke_type> middleware_invokes_;
    };

    struct static_route_slot final {
        std::uint64_t hash_{};
        std::size_t route_index_{no_route_index};
    };

    struct dynamic_node_type;

    struct dynamic_static_child_type final {
        std::pmr::string segment_;
        dynamic_node_type* node_{nullptr};
    };

    struct dynamic_node_type final {
        explicit dynamic_node_type(std::pmr::memory_resource* resource)
            : static_children_(resource) {}

        std::pmr::vector<dynamic_static_child_type> static_children_;
        dynamic_node_type* param_child_{nullptr};
        std::size_t route_index_{no_route_index};
        std::size_t wildcard_route_index_{no_route_index};
    };

    struct connect_protocol_index_type final {
        connect_protocol_index_type(std::pmr::memory_resource* resource, std::string_view value)
            : protocol_(value, resource),
              root_(resource) {}
        std::pmr::string protocol_;
        dynamic_node_type root_;
    };
    std::pmr::memory_resource* resource_;
    std::pmr::vector<route_identity_type> identities_;
    std::pmr::vector<std::size_t> extension_route_indices_;
    std::pmr::vector<std::size_t> connect_route_indices_;
    std::pmr::vector<connect_protocol_index_type> connect_protocols_;
    std::pmr::vector<static_route_slot> static_slots_;
    std::array<dynamic_node_type, routable_method_count> dynamic_roots_;
    std::pmr::vector<dynamic_node_type> dynamic_node_arena_;
    std::pmr::vector<route_middleware_type::invoke_type> unmatched_middleware_invokes_;
    std::uint32_t static_method_mask_{0};
    std::uint32_t dynamic_method_mask_{0};
    std::uint32_t allowed_method_mask_{0};
    std::size_t static_slot_mask_{};
    bool has_route_rate_limit_{false};
};

using compiled_route_plan_ptr_type =
    std::unique_ptr<compiled_route_plan, pmr_object_deleter<compiled_route_plan>>;

}  // namespace ruvia::detail
