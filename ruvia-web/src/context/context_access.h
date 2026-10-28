#pragma once

#include <cstddef>
#include <exception>
#include <optional>
#include <utility>

#include "ruvia/web/context.h"

#include "context/context_request_storage.h"
#include "context/context_services.h"
#include "context/http_interim_response_output.h"
#include "http/static_file_variant.h"

namespace ruvia::detail {

class context_websocket_binding;
class context_tunnel_binding;

struct context_access final {
    [[nodiscard]] static context make(
        request_memory& memory, const http_request& request, const context_services& services) {
        return context(memory, request, services);
    }

    [[nodiscard]] static context make(request_memory& memory, const http_request& request,
        std::string_view route_path, const std::string_view* param_names,
        const std::string_view* param_values, std::size_t param_count,
        std::uintptr_t route_rate_limit_scope, const context_services& services) {
        return context(memory, request, route_path, param_names, param_values, param_count,
            route_rate_limit_scope, services);
    }

    [[nodiscard]] static http_response static_file_with_precompressed_variants(
        context& context_value, const static_root& root, static_file_response_options options) {
        return context_value.static_file(root, options, static_file_selection_mode::precompressed);
    }

    [[nodiscard]] static http_response make_static_json_response(
        const context& context_value, std::string_view json_body) {
        http_response response(context_value.arena());
        response.content_type(http_content_type::application_json);
        response.static_body(json_body);
        context_value.apply_response_state(response, std::nullopt);
        return response;
    }

    [[nodiscard]] static http_response make_owned_json_response(
        const context& context_value, std::pmr::string& json_body) {
        return context_value.json_serialized(json_body);
    }

    [[nodiscard]] static http_interim_response_output* interim_output(context& context_value) noexcept {
        return context_value.services().interim_output();
    }

    [[nodiscard]] static const http_request& request(const context& context_value) noexcept {
        return context_value.request_;
    }

    [[nodiscard]] static detail::rate_limiter* rate_limiter(context& context_value) noexcept {
        return context_value.services().rate_limiter();
    }

    [[nodiscard]] static std::uintptr_t route_rate_limit_scope(const context& context_value) noexcept {
        return context_value.route_rate_limit_scope_;
    }

    static void set_response(context& context_value, http_response&& response) {
        context_value.store_response(std::move(response));
    }

    [[nodiscard]] static http_response& response_storage(context& context_value) {
        return context_value.response_storage();
    }

    // The metadata overlay of a final response, or nullptr before one exists.
    // Header changes staged after a response became final must land here as
    // well as on the response itself, so a replacement built afterwards (an
    // outer middleware's respond(), an error handler) still carries them; see
    // context_response_state::update_metadata.
    [[nodiscard]] static http_response* final_response_metadata(context& context_value) noexcept {
        auto* final_state = context_value.response_state().final();
        return final_state == nullptr ? nullptr : &final_state->metadata();
    }

    [[nodiscard]] static bool has_response_header(
        const context& context_value, std::string_view name) noexcept {
        return context_value.response_state().active_response().header(name).has_value();
    }

    // The status the handler set; a file response sends it for the full body.
    [[nodiscard]] static http_status_code response_status(const context& context_value) noexcept {
        return context_value.response_state().metadata().status();
    }

    static void apply_response_state(const context& context_value, http_response& response,
        std::optional<http_status_code> status_code) {
        context_value.apply_response_state(response, status_code);
    }

    // Called by a driver right before it hands the upgrade/CONNECT handshake
    // head to the transport: from here on no HTTP response can follow, and no
    // 1xx head either -- the stream now carries the upgraded protocol. Throws,
    // before anything is written, while an interim head is still being written.
    static void commit_upgrade(context& context_value) {
        if (auto* interim = context_value.services().interim_output()) {
            interim->commit_final();
        }
        context_value.response_output() = context_response_output::upgraded();
    }

    [[nodiscard]] static bool output_committed(const context& context_value) noexcept {
        return context_value.response_output().committed();
    }

    static void set_error(context& context_value, std::exception_ptr exception) noexcept {
        context_value.store_error(std::move(exception));
    }

    [[nodiscard]] static bool has_response(const context& context_value) noexcept {
        return context_value.has_response();
    }

    // The final response, for the dispatcher to return; see
    // context_response_state::release_final.
    [[nodiscard]] static http_response&& release_response(context& context_value) noexcept {
        return context_value.response_state().release_final();
    }

    [[nodiscard]] static http_response streaming_head(const context& context_value) {
        return context_value.streaming_head();
    }

    // Commits the bound session, then builds the head a response stream sends before its first byte.
    [[nodiscard]] static task<http_response> commit_streaming_head(context& context_value);

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
