#pragma once

#include <cstddef>
#include <exception>
#include <utility>

#include "ruvia/web/context.h"

#include "context/context_request_storage.h"
#include "context/context_services.h"
#include "http/static_file_variant.h"

namespace ruvia::detail {

class context_websocket_binding;
class context_tunnel_binding;

struct context_access final {
    [[nodiscard]] static context make(
        request_memory& memory, const http_request& request, context_services services) {
        return context(memory, request, services);
    }

    [[nodiscard]] static context make(request_memory& memory, const http_request& request,
        std::uintptr_t route_rate_limit_scope, context_services services) {
        return context(memory, request, {}, nullptr, nullptr, 0, route_rate_limit_scope, services);
    }

    [[nodiscard]] static context make(request_memory& memory, const http_request& request,
        std::string_view route_path, std::uintptr_t route_rate_limit_scope,
        context_services services) {
        return context(
            memory, request, route_path, nullptr, nullptr, 0, route_rate_limit_scope, services);
    }

    [[nodiscard]] static context make(request_memory& memory, const http_request& request,
        std::string_view route_path, const std::string_view* param_names,
        const std::string_view* param_values, std::size_t param_count,
        std::uintptr_t route_rate_limit_scope, context_services services) {
        return context(memory, request, route_path, param_names, param_values, param_count,
            route_rate_limit_scope, services);
    }

    [[nodiscard]] static http_response static_file_with_precompressed_variants(
        context& context_value, const static_root& root, static_file_response_options options) {
        return context_value.static_file(root, options, static_file_selection_mode::precompressed);
    }

    [[nodiscard]] static http_interim_response_output* interim_output(context& context_value) noexcept {
        return context_value.services().interim_output();
    }

    [[nodiscard]] static const http_request& request(const context& context_value) noexcept {
        return context_value.request_;
    }

    [[nodiscard]] static rate_limiter_type* rate_limiter(context& context_value) noexcept {
        return context_value.services().rate_limiter();
    }

    [[nodiscard]] static std::uintptr_t route_rate_limit_scope(const context& context_value) noexcept {
        return context_value.route_rate_limit_scope_;
    }

    [[nodiscard]] static bool request_cookies_materialized(const context& context_value) noexcept {
        return context_value.request_storage_->cookies_.has_value();
    }

    [[nodiscard]] static bool request_query_materialized(const context& context_value) noexcept {
        return context_value.request_storage_->query_.has_value();
    }

    [[nodiscard]] static bool route_params_materialized(const context& context_value) noexcept {
        return context_value.request_storage_->route_params_.has_value();
    }

    [[nodiscard]] static const context_request_storage* request_storage(
        const context& context_value) noexcept {
        return context_value.request_storage_.get();
    }

    static void set_response(context& context_value, http_response&& response) {
        context_value.store_response(std::move(response));
    }

    [[nodiscard]] static http_response& response_storage(context& context_value) {
        return context_value.response_storage();
    }

    [[nodiscard]] static bool has_response_header(
        const context& context_value, std::string_view name) noexcept {
        return context_value.response_state().active_response().header(name).has_value();
    }

    static void mark_websocket_handshake_started(context& context_value) noexcept {
        context_value.request_storage().websocket_handshake_started_ = true;
    }

    [[nodiscard]] static bool websocket_handshake_started(const context& context_value) noexcept {
        return context_value.request_storage().websocket_handshake_started_;
    }

    static void mark_tunnel_handshake_started(context& context_value) noexcept {
        context_value.request_storage().tunnel_handshake_started_ = true;
    }
    [[nodiscard]] static bool tunnel_handshake_started(const context& context_value) noexcept {
        return context_value.request_storage().tunnel_handshake_started_;
    }
    static void set_error(context& context_value, std::exception_ptr exception) noexcept {
        context_value.store_error(std::move(exception));
    }

    [[nodiscard]] static bool has_response(const context& context_value) noexcept {
        return context_value.has_response();
    }

    [[nodiscard]] static http_response take_response(context& context_value) {
        return context_value.take_response();
    }

    [[nodiscard]] static http_response streaming_head(
        const context& context_value, std::string_view content_type = {}) {
        return context_value.streaming_head(content_type);
    }

    // Sets a pending response header on the context, as a handler would before
    // streaming. Lets a test seed e.g. a caller-provided Cache-Control that the
    // stream-head builder must then honor.
    static void set_response_header(context& context_value, std::string_view name, std::string_view value) {
        context_value.set_stable_response_header(name, value);
    }

    // True if a Set-Cookie whose value begins with `value_prefix` (e.g. a cookie
    // name plus '=') is already queued on the context's pending response headers.
    // Lets a test observe a cookie set by middleware before any response is built.
    [[nodiscard]] static bool has_pending_set_cookie(
        const context& context_value, std::string_view value_prefix) noexcept {
        return !pending_set_cookie_value(context_value, value_prefix).empty();
    }

    [[nodiscard]] static std::string_view pending_set_cookie_value(
        const context& context_value, std::string_view value_prefix) noexcept {
        for (const auto& header : context_value.response_state().active_response().headers()) {
            if (header.name() == "Set-Cookie" && header.value().starts_with(value_prefix)) {
                return header.value();
            }
        }
        return {};
    }

private:
    friend class context_websocket_binding;
    friend class context_tunnel_binding;
    [[nodiscard]] static context_response_output bind_tunnel(context& context_value, http_tunnel& tunnel) noexcept {
        auto previous = context_value.response_output();
        context_value.response_output() = context_response_output::tunnel(tunnel);
        return previous;
    }

    [[nodiscard]] static context_response_output bind_websocket(
        context& context_value, websocket& websocket_value) noexcept {
        auto previous = context_value.response_output();
        context_value.response_output() = context_response_output::websocket_value(websocket_value);
        return previous;
    }

    static void restore_response_output(context& context_value, context_response_output output) noexcept {
        context_value.response_output() = output;
    }
};

class context_tunnel_binding final {
public:
    context_tunnel_binding(context& context_value, http_tunnel& tunnel) noexcept
        : context_(context_value),
          previous_(context_access::bind_tunnel(context_value, tunnel)) {}
    context_tunnel_binding(const context_tunnel_binding&) = delete;
    context_tunnel_binding& operator=(const context_tunnel_binding&) = delete;
    ~context_tunnel_binding() {
        context_access::restore_response_output(context_, previous_);
    }

private:
    context& context_;
    context_response_output previous_;
};

// The facade borrowed by context is valid only while the established session
// owns its connection. Restoring the previous output capability on every exit
// prevents onion middleware post-processing from observing a dangling facade.
class context_websocket_binding final {
public:
    context_websocket_binding(context& context_value, websocket& websocket_value) noexcept
        : context_(&context_value),
          previous_(context_access::bind_websocket(context_value, websocket_value)) {}

    context_websocket_binding(const context_websocket_binding&) = delete;
    context_websocket_binding& operator=(const context_websocket_binding&) = delete;
    context_websocket_binding(context_websocket_binding&&) = delete;
    context_websocket_binding& operator=(context_websocket_binding&&) = delete;

    ~context_websocket_binding() {
        context_access::restore_response_output(*context_, previous_);
    }

private:
    context* context_;
    context_response_output previous_;
};

}  // namespace ruvia::detail
