#pragma once

#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory_resource>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "ruvia/http/http_known_method.h"
#include "ruvia/http/http_response.h"
#include "ruvia/web/context.h"
#include "ruvia/web/detail/router/route_handlers.h"
#include "ruvia/web/error.h"
#include "ruvia/web/error_handlers.h"
#include "ruvia/web/next.h"

#include "context/context_services.h"
#include "http/static_file_variant.h"
#include "router/compiled_route_plan.h"
#include "router/middleware_chain.h"
#include "router/route_entry.h"
#include "router/route_resolution.h"
#include "server/document_root_binding.h"

namespace ruvia::detail {

class controller_store;
class router_impl;

// One path-prefix-scoped fallback registration (Hono sub-app scoping analog).
// The prefix is a borrowed view during registration; the compiled plan copies
// it into owned storage. Selection is deepest-prefix-first on whole path
// segments (path_is_under_prefix).
struct http_prefix_error_handler final {
    std::string_view prefix_;
    http_error_handler_ref_type handler_{nullptr};
};

struct http_prefix_not_found_handler final {
    std::string_view prefix_;
    http_not_found_handler_ref_type handler_{nullptr};
};

// One worker's binding of the process route plan: the worker's controller
// instance behind every route and the worker's middleware instance behind
// every frame, indexed exactly like the plan. Everything else -- lookup,
// endpoint metadata, limits, fallbacks -- is read from the shared plan.
class route_table final {
public:
    // The plan and the controllers must outlive the table. Middleware
    // instances are created here and destroyed with the table.
    route_table(std::pmr::memory_resource* resource, const compiled_route_plan& plan,
        const controller_store& controllers);
    ~route_table();
    route_table(const route_table&) = delete;
    route_table& operator=(const route_table&) = delete;
    route_table(route_table&&) = delete;
    route_table& operator=(route_table&&) = delete;

    [[nodiscard]] bool has_route_rate_limit() const noexcept {
        return plan_->has_route_rate_limit_;
    }
    // Builds a request path from a registered route pattern: ":name" segments
    // take the next value (percent-encoded, non-empty), a trailing "*" takes
    // the final value (slashes preserved, may be empty). The pattern is the
    // route's identity -- an unregistered pattern, a value-count mismatch, a
    // "."/".." segment, or a pattern starting with an empty segment (a "//"
    // network-path reference) is a programming error and throws
    // std::invalid_argument. A root "*" value's leading slash becomes "%2F".
    [[nodiscard]] std::pmr::string url_for(std::string_view pattern,
        std::span<const std::string_view> values, std::pmr::memory_resource* resource) const;
    [[nodiscard]] route_resolution resolve(const http_request& request) const noexcept;

    // Tokens of the extension routes registered on `path`, for the Allow header
    // of a 405. Written into caller storage so no allocation outlives the call.
    [[nodiscard]] std::span<const std::string_view> extension_methods_for(
        std::string_view path, std::span<std::string_view> buffer) const noexcept;
    [[nodiscard]] bool has_extension_routes_for(std::string_view path) const noexcept;

    // Whether ANY route in the table uses this exact token. RFC 9110 15.5.6
    // makes 405 conditional on the method being "known by the origin server",
    // so a token nobody registered is 501 no matter what the target path holds.
    [[nodiscard]] bool recognizes_method_token(std::string_view method_token) const noexcept;
    task<http_response> dispatch(const http_request& request, const route_resolution& resolution,
        request_memory& memory, context_services services) const;
    // Canonical buffered-response application dispatch. An unresolved route
    // first consults the configured document
    // root, then falls through to 404/405/OPTIONS handling. Any failure escaping
    // the routing machinery becomes an error response. Connection persistence
    // and wire framing remain the HTTP/1 driver's responsibility. Pass
    // document_root_binding::none() when no root is configured, or the
    // worker's document_root_binding::configured(generation) lease otherwise.
    task<http_response> dispatch_buffered_response(const http_request& request,
        const route_resolution& resolution, request_memory& memory, document_root_binding document_root,
        context_services services, static_file_selection_mode static_file_mode) const;
    task<http_response> handle_error(const http_request& request, request_memory& memory,
        http_error_info error, context_services services) const;
    task<http_response> handle_exception(const http_request& request, request_memory& memory,
        std::exception_ptr exception, context_services services) const;
    // Absence means the bound output handled the request; a value is the one
    // buffered response produced before a response-stream commit.
    task<std::optional<http_response>> dispatch_response_stream(const http_request& request,
        const resolved_route& route, request_memory& memory, response_stream_writer& response_stream,
        context_services services) const;
    // A CONNECT tunnel or WebSocket upgrade: `handler` is the terminal that
    // commits the upgrade. Absence means the terminal took the request.
    task<std::optional<http_response>> dispatch_upgrade(const http_request& request,
        const resolved_route& route, request_memory& memory, const route_stream_handler_type& handler,
        context_services services) const;
    // The not-found handler that applies to `path`: the tightest prefix scope
    // covering it, else the plan-wide one; null when neither is configured.
    [[nodiscard]] http_not_found_handler_ref_type not_found_handler_for(std::string_view path) const noexcept;

private:
    friend class router_impl;

    [[nodiscard]] route_resolution resolve(
        http_known_method method, std::string_view path) const noexcept;
    [[nodiscard]] route_resolution resolve_connect(std::string_view protocol, std::string_view target) const noexcept;
    // Extension-method routing, kept off every enum-indexed structure. The
    // request's exact token is compared against a small cold list, which costs
    // a known-method request nothing: the HTTP/1 driver only reaches it when
    // classify_http_method() returned unknown.
    [[nodiscard]] route_resolution resolve_extension_method(
        std::string_view method_token, std::string_view path) const noexcept;
    [[nodiscard]] route_resolution resolved(const route_entry& route, const route_match& match = {}) const noexcept;

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

    [[nodiscard]] static std::size_t method_index(http_known_method method) noexcept;
    [[nodiscard]] static bool is_routable_method(http_known_method method) noexcept;
    [[nodiscard]] static bool supports_head_fallback(const route_entry& route) noexcept;
    [[nodiscard]] static std::uint64_t path_hash(std::string_view path) noexcept;
    [[nodiscard]] static std::uint64_t route_hash(
        http_known_method method, std::uint64_t hash) noexcept;
    [[nodiscard]] static std::size_t find_dynamic_node(
        const dynamic_node_type& node, std::string_view path, route_match& match) noexcept;
    [[nodiscard]] static std::size_t find_dynamic_node_no_params(
        const dynamic_node_type& node, std::string_view path) noexcept;
    [[nodiscard]] static const dynamic_static_child_type* find_dynamic_static_child(
        const dynamic_node_type& node, std::string_view segment) noexcept;

    [[nodiscard]] const route_entry* find_static_route(
        http_known_method method, std::string_view path, std::uint64_t hash) const noexcept;
    [[nodiscard]] const route_entry* find_dynamic_route(
        http_known_method method, std::string_view path, route_match& match) const noexcept;
    [[nodiscard]] std::uint32_t allowed_methods(
        std::string_view path, http_known_method requested_method, std::uint64_t hash) const noexcept;
    [[nodiscard]] task<http_response> dispatch_request(const http_request& request,
        const route_resolution& resolution, request_memory& memory, context_services services,
        document_root_binding document_root, dispatch_failure_type failure,
        static_file_selection_mode static_file_mode) const;
    [[nodiscard]] task<http_response> invoke_route(const resolved_route& resolved, context& context) const;
    // The handler is copied into the coroutine frame: callers may pass one
    // bound from the resolved route as a temporary.
    [[nodiscard]] task<std::optional<http_response>> dispatch_stream_route(const http_request& request,
        const resolved_route& route, request_memory& memory, route_stream_handler_type handler,
        context_services services) const;

    // A route's own frames and, only when some are conditional, their scopes.
    [[nodiscard]] middleware_chain route_chain(const route_entry& route) const noexcept;
    // Runs `frames` around `terminal`, whose response becomes the result; the
    // completion check shared by matched buffered routes and the unmatched
    // chain.
    [[nodiscard]] task<http_response> respond_through_chain(
        middleware_chain chain, context& context, route_handler_type terminal) const;
    // Runs the unmatched-request middleware chain around `terminal`, which
    // produces the 404/405/501 response. Falls straight through to the terminal
    // when nothing declared itself for unmatched requests.
    [[nodiscard]] task<http_response> run_unmatched_chain(context& context, route_handler_type terminal) const;
    [[nodiscard]] static task<void> contain_failure(
        const route_table& table, context& context, std::exception_ptr exception);
    [[nodiscard]] static task<void> contain_stream_failure(
        const route_table& table, context& context, std::exception_ptr exception);
    [[nodiscard]] task<http_response> handle_error(context& context, http_error_info error) const;
    [[nodiscard]] task<http_response> handle_not_found(
        const http_request& request, request_memory& memory, context_services services) const;
    [[nodiscard]] task<http_response> handle_exception(
        context& context, std::exception_ptr exception) const;

    [[nodiscard]] http_error_handler_ref_type error_handler_for(std::string_view path) const noexcept;

    std::pmr::memory_resource* resource_;
    const compiled_route_plan* plan_;
    // Worker-bound, indexed like the plan: the controller instance behind each
    // route, the middleware instances this table owns, and each frame bound to
    // its instance.
    std::pmr::vector<void*> handler_targets_;
    std::pmr::vector<void*> middleware_instances_;
    std::pmr::vector<route_middleware_type> middleware_frames_;
};

}  // namespace ruvia::detail
