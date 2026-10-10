#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory_resource>
#include <new>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ruvia/core/memory/pmr_resource.h"
#include "ruvia/http/http_known_method.h"
#include "ruvia/http/http_response.h"
#include "ruvia/web/context.h"
#include "ruvia/web/detail/util/callable_ref.h"
#include "ruvia/web/error.h"
#include "ruvia/web/error_handlers.h"
#include "ruvia/web/next.h"
#include "ruvia/web/websocket.h"

#include "context/context_services.h"
#include "http/static_file_variant.h"
#include "router/compiled_route_plan.h"
#include "router/route_entry.h"
#include "router/route_resolution.h"
#include "router/router.h"
#include "server/document_root_binding.h"

namespace ruvia {
class static_root;
}

namespace ruvia::detail {

class db_registry;
class redis_registry;
class router_impl;

struct next_access final {
    [[nodiscard]] static constexpr next make(next_state state_value, next_invoke_type invoke) noexcept {
        return next(state_value, invoke);
    }

    [[nodiscard]] static next& make_in(
        std::pmr::memory_resource* resource, next_state state_value, next_invoke_type invoke) {
        auto* resolved = pmr_resource_or_default(resource);
        auto* storage = resolved->allocate(sizeof(next), alignof(next));
        return *new (storage) next(state_value, invoke);
    }
};

// One path-prefix-scoped fallback registration (Hono sub-app scoping analog).
// The prefix is a borrowed view during registration; route_table copies it into
// owned storage. Selection is deepest-prefix-first on whole path segments
// (path_is_under_prefix).
struct http_prefix_error_handler final {
    std::string_view prefix_;
    http_error_handler_ref_type handler_{nullptr};
};

struct http_prefix_not_found_handler final {
    std::string_view prefix_;
    http_not_found_handler_ref_type handler_{nullptr};
};

// Normalized borrowed facts, valid only for this lookup. Protocol adapters
// retain ownership and decide admission/body lifetime independently of routing.
struct route_request_view final {
    http_known_method known_method_{http_known_method::unknown};
    std::string_view method_token_;
    std::string_view path_;
    std::string_view authority_;
    std::string_view extended_protocol_;
};

class route_table final {
public:
    explicit route_table(std::pmr::memory_resource* resource);
    route_table(const route_table&) = delete;
    route_table& operator=(const route_table&) = delete;
    route_table(route_table&&) = delete;
    route_table& operator=(route_table&&) = delete;

    void set_error_handler(http_error_handler_ref_type handler) noexcept;
    void set_not_found_handler(http_not_found_handler_ref_type handler) noexcept;
    // Wholesale replacement (idempotent for an app stop()/run() cycle). The
    // stored set is normalized (trailing slash stripped) and ordered deepest
    // prefix first so selection is a first-match scan.
    void set_prefix_error_handlers(std::span<const http_prefix_error_handler> handlers);
    void set_prefix_not_found_handlers(std::span<const http_prefix_not_found_handler> handlers);
    [[nodiscard]] bool has_route_rate_limit() const noexcept {
        return has_route_rate_limit_;
    }
    // Builds a request path from a registered route pattern: ":name" segments
    // take the next value (percent-encoded, non-empty), a trailing "*" takes
    // the final value (slashes preserved, may be empty). The pattern is the
    // route's identity -- an unregistered pattern or a value-count mismatch is
    // a programming error and throws std::invalid_argument.
    [[nodiscard]] std::pmr::string url_for(std::string_view pattern,
        std::span<const std::string_view> values, std::pmr::memory_resource* resource) const;
    [[nodiscard]] route_resolution resolve(const http_request& request) const noexcept;
    [[nodiscard]] route_resolution resolve(route_request_view request) const noexcept;
    [[nodiscard]] route_resolution resolve(
        http_known_method method, std::string_view path) const noexcept;

    task<std::optional<http_response>> dispatch_tunnel(const http_request& request,
        const resolved_route& resolved, request_memory& memory, const route_stream_handler_type& handler,
        context_services services) const;

    // Tokens of the extension routes registered on `path`, for the Allow header
    // of a 405. Written into caller storage so no allocation outlives the call.
    [[nodiscard]] std::span<const std::string_view> extension_methods_for(
        std::string_view path, std::span<std::string_view> buffer) const noexcept;
    // Unique extension method tokens registered anywhere on the server, for the
    // server-wide Allow header of OPTIONS *.
    [[nodiscard]] std::span<const std::string_view> extension_methods_for_server() const noexcept;
    [[nodiscard]] bool has_extension_routes_for(std::string_view path) const noexcept;

    // Whether ANY route in the table uses this exact token. RFC 9110 15.5.6
    // makes 405 conditional on the method being "known by the origin server",
    // so a token nobody registered is 501 no matter what the target path holds.
    [[nodiscard]] bool recognizes_method_token(std::string_view method_token) const noexcept;
    task<http_response> dispatch(
        const http_request& request, request_memory& memory, context_services services) const;
    task<http_response> dispatch(const http_request& request, const route_resolution& resolution,
        request_memory& memory, context_services services) const;
    // Canonical buffered-response application dispatch for every server
    // protocol. An unresolved route first consults the configured document
    // root, then falls through to 404/405/OPTIONS handling. Any failure escaping
    // the routing machinery becomes an error response. Connection persistence
    // and wire framing remain the protocol driver's responsibility. Pass
    // document_root_binding::none() when no root is configured,
    // document_root_binding::standalone(root) for an immutable root, or
    // document_root_binding::configured(root) for a server-owned refreshing root.
    task<http_response> dispatch_buffered_response(const http_request& request,
        const route_resolution& resolution, request_memory& memory, document_root_binding document_root,
        context_services services,
        static_file_selection_mode static_file_mode = static_file_selection_mode::identity_only) const;
    task<http_response> handle_error(const http_request& request, request_memory& memory,
        http_error_info error, context_services services) const;
    task<http_response> handle_exception(const http_request& request, request_memory& memory,
        std::exception_ptr exception, context_services services) const;
    // Absence means the bound output handled the request; a value is the one
    // buffered response produced before a response-stream commit.
    task<std::optional<http_response>> dispatch_response_stream(const http_request& request,
        const resolved_route& route, request_memory& memory, response_stream_writer& response_stream,
        context_services services) const;
    task<std::optional<http_response>> dispatch_websocket(const http_request& request,
        const resolved_route& route, request_memory& memory, const route_stream_handler_type& handler,
        context_services services) const;

    // application moves the first worker's compiled plan to process ownership, then
    // binds every later worker table to that same immutable lookup structure.
    [[nodiscard]] compiled_route_plan_ptr_type release_compiled_plan();

private:
    friend class router_impl;
    [[nodiscard]] route_resolution resolve_connect(std::string_view protocol, std::string_view target) const noexcept;
    // Extension-method routing, kept off every enum-indexed structure. The
    // request's exact token is compared against a small cold list, which costs
    // a known-method request nothing: both protocol drivers only reach it when
    // classify_http_method() returned unknown.
    [[nodiscard]] route_resolution resolve_extension_method(
        std::string_view method_token, std::string_view path) const noexcept;

    // How dispatch_request treats a failure escaping the routing machinery
    // itself. Handler exceptions are already converted inside the route path.
    enum class dispatch_failure_type : std::uint8_t {
        propagate,
        respond,
    };

    static constexpr std::size_t routable_method_count = compiled_route_plan::routable_method_count;
    static constexpr std::size_t no_route_index = compiled_route_plan::no_route_index;
    using dynamic_node_type = compiled_route_plan::dynamic_node_type;
    using dynamic_static_child_type = compiled_route_plan::dynamic_static_child_type;

    void build_static_index();
    void build_dynamic_routes();
    void build_allowed_method_mask();
    void bind_compiled_plan(const compiled_route_plan& plan);
    void capture_route_identities();
    void bind_dynamic_param_names();
    void bind_dynamic_param_names(route_entry& route);
    void build_server_extension_method_tokens();

    [[nodiscard]] static std::size_t method_index(http_known_method method) noexcept;
    [[nodiscard]] static bool is_routable_method(http_known_method method) noexcept;
    [[nodiscard]] static bool supports_head_fallback(const route_entry& route) noexcept;
    [[nodiscard]] static bool is_dynamic_path(std::string_view path) noexcept;
    [[nodiscard]] static std::uint64_t path_hash(std::string_view path) noexcept;
    [[nodiscard]] static std::uint64_t route_hash(
        http_known_method method, std::uint64_t hash) noexcept;
    [[nodiscard]] static std::size_t dynamic_node_upper_bound(std::string_view path) noexcept;
    [[nodiscard]] static std::size_t dynamic_param_name_upper_bound(std::string_view path) noexcept;
    void insert_dynamic(dynamic_node_type& root, route_entry& route, std::size_t route_index);
    void append_dynamic_param_name(route_entry& route, std::string_view name);
    static void sort_dynamic_node(dynamic_node_type& node);
    [[nodiscard]] static std::size_t find_dynamic_node(
        const dynamic_node_type& node, std::string_view path, route_match& match) noexcept;
    [[nodiscard]] static std::size_t find_dynamic_node_no_params(
        const dynamic_node_type& node, std::string_view path) noexcept;
    [[nodiscard]] static const dynamic_static_child_type* find_dynamic_static_child(
        const dynamic_node_type& node, std::string_view segment) noexcept;
    [[nodiscard]] static bool add_param(route_match& match, std::string_view value) noexcept;
    [[nodiscard]] static bool same_dynamic_shape(
        std::string_view left, std::string_view right) noexcept;

    [[nodiscard]] const route_entry* find_static_route(
        http_known_method method, std::string_view path, std::uint64_t hash) const noexcept;
    [[nodiscard]] const route_entry* find_dynamic_route(
        http_known_method method, std::string_view path, route_match& match) const noexcept;
    [[nodiscard]] const route_entry* find_dynamic(
        http_known_method method, std::string_view path, route_match& match) const noexcept;
    [[nodiscard]] std::uint32_t allowed_methods(
        std::string_view path, http_known_method requested_method, std::uint64_t hash) const noexcept;
    [[nodiscard]] std::uint32_t allowed_methods_for_server() const noexcept;
    [[nodiscard]] task<http_response> dispatch_request(const http_request& request,
        const route_resolution& resolution, request_memory& memory, context_services services,
        document_root_binding document_root, dispatch_failure_type failure,
        static_file_selection_mode static_file_mode) const;
    [[nodiscard]] task<http_response> invoke_route(const route_entry& route, context& context) const;
    [[nodiscard]] task<http_response> invoke_route_with_middleware(
        const route_entry& route, context& context) const;
    [[nodiscard]] task<void> invoke_middleware_at(
        const route_entry& route, std::size_t index, context& context) const;
    [[nodiscard]] static task<void> invoke_middleware_continuation(next_state state);
    // The first frame at or after `index` that applies to this request: a
    // conditional frame is skipped when the request path is outside its scope.
    // Routes without conditional frames return `index` untouched.
    [[nodiscard]] std::size_t applicable_middleware_index(
        const route_entry& route, std::size_t index, const context& context) const noexcept;
    [[nodiscard]] task<std::optional<http_response>> dispatch_stream_route(const http_request& request,
        const resolved_route& route, request_memory& memory, const route_stream_handler_type& handler,
        context_services services) const;
    [[nodiscard]] task<void> invoke_stream_middleware_at(const route_entry& route, std::size_t index,
        context& context, stream_middleware_chain_state& chain,
        const route_stream_handler_type& handler) const;
    [[nodiscard]] static task<void> invoke_stream_middleware_continuation(next_state state);
    [[nodiscard]] task<void> store_middleware_exception_response(
        context& context, std::exception_ptr exception) const;
    [[nodiscard]] task<http_response> handle_error(context& context, http_error_info error) const;
    [[nodiscard]] task<http_response> handle_not_found(
        const http_request& request, request_memory& memory, context_services services) const;

    // Runs the unmatched-request middleware chain around `terminal`, which
    // produces the 404/405/501 response. Falls straight through to the terminal
    // when nothing declared itself for unmatched requests.
    using unmatched_terminal_type = callable_ref<http_response, context&>;
    [[nodiscard]] task<http_response> run_unmatched_chain(
        context& context, const unmatched_terminal_type& terminal) const;
    [[nodiscard]] task<void> invoke_unmatched_middleware_at(
        std::size_t index, context& context, const unmatched_terminal_type& terminal) const;
    [[nodiscard]] static task<void> invoke_unmatched_middleware_continuation(next_state state);
    [[nodiscard]] task<http_response> handle_exception(
        context& context, std::exception_ptr exception) const;

    template <typename handler_type>
    struct stored_prefix_handler_type final {
        stored_prefix_handler_type(
            std::pmr::memory_resource* resource, std::string_view prefix_value, handler_type handler_value)
            : prefix_(prefix_value, resource),
              handler_(handler_value) {}

        std::pmr::string prefix_;
        handler_type handler_{nullptr};
    };

    [[nodiscard]] http_error_handler_ref_type error_handler_for(std::string_view path) const noexcept;
    [[nodiscard]] http_not_found_handler_ref_type not_found_handler_for(std::string_view path) const noexcept;

    std::pmr::memory_resource* resource_;
    std::pmr::vector<route_entry> routes_;
    std::pmr::vector<route_middleware_type> middleware_frames_;
    // Parallel to middleware_frames_: the path scope a conditional frame checks
    // at dispatch, or empty for an unconditional frame. Only routes flagged
    // has_conditional_middleware() read it. Views borrow the registration-owned
    // use_at prefixes, which outlive the table.
    std::pmr::vector<std::string_view> middleware_scopes_;
    // A contiguous block at the end of middleware_frames_: the app-wide
    // middlewares that declared they also run when no route matched. Kept as a
    // range rather than a separate vector so the fallback chain indexes frames
    // exactly the way a route's chain does.
    // Indices into routes_ rather than pointers: routes_ is populated in two
    // passes and this is built after both.
    std::pmr::vector<std::string_view> server_extension_method_tokens_;
    std::size_t unmatched_middleware_offset_{0};
    std::size_t unmatched_middleware_count_{0};
    std::pmr::vector<std::string_view> dynamic_param_names_;
    compiled_route_plan_ptr_type owned_plan_;
    const compiled_route_plan* plan_{nullptr};
    http_error_handler_ref_type error_handler_{nullptr};
    http_not_found_handler_ref_type not_found_handler_{nullptr};
    std::pmr::vector<stored_prefix_handler_type<http_error_handler_ref_type>> prefix_error_handlers_{resource_};
    std::pmr::vector<stored_prefix_handler_type<http_not_found_handler_ref_type>> prefix_not_found_handlers_{
        resource_};
    bool has_route_rate_limit_{false};
};

}  // namespace ruvia::detail
