#include <cstdint>
#include <optional>
#include <span>
#include <utility>

#include "ruvia/web/static_files.h"

#include "context/context_access.h"
#include "router/route_context.h"
#include "router/route_table.h"

// Choosing what answers a request: the matched route, a 405 with Allow, the
// document-root fallback, or nothing -- and running a buffered handler once one
// is chosen.

namespace ruvia {

namespace {

http_response make_allow_no_content_response(request_memory& memory, std::uint32_t method_mask,
    std::span<const std::string_view> extension_methods = {}) {
    http_response response({.resource_ = memory.resource()});
    response.status(ruvia::http_status::no_content);
    response.allow_methods(method_mask, extension_methods);
    return response;
}

[[nodiscard]] std::optional<http_response> select_document_root_fallback(
    const detail::document_root_binding& document_root, const http_request& request,
    request_memory& memory, const detail::context_services& services,
    detail::static_file_selection_mode static_file_mode) {
    const auto* const root = document_root.root();
    if (root == nullptr || (request.known_method() != http_known_method::get &&
                               request.known_method() != http_known_method::head)) {
        return std::nullopt;
    }

    // resolve() rejects every target that is not an origin path naming a
    // resource, so a probed path here is origin-form and dot-segment free.
    auto relative = request.path();
    relative.remove_prefix(1);

    auto context_value = detail::context_access::make(memory, request, services);
    try {
        if (static_file_mode == detail::static_file_selection_mode::precompressed) {
            return detail::context_access::static_file_with_precompressed_variants(
                context_value, *root, {.relative_path_ = relative});
        }
        return context_value.static_file(*root, {.relative_path_ = relative});
    } catch (const http_error& error) {
        // A document-root miss is allowed to fall through to the router's
        // normal not-found path. A real response error (for example 406 when
        // every Accept-Encoding choice is forbidden, or 412 from a file
        // precondition) must retain its status instead of being rewritten as
        // 404 by the fallback probe.
        const auto status = error.info().status();
        if (status == ruvia::http_status::forbidden || status == ruvia::http_status::not_found) {
            return std::nullopt;
        }
        throw;
    }
}

}  // namespace

task<std::optional<http_response>> detail::route_table::dispatch_response_stream(
    const http_request& request, const resolved_route& resolved, request_memory& memory,
    response_stream_writer& response_stream, context_services services) const {
    if (resolved.route().endpoint().response_stream() == nullptr) {
        throw std::logic_error("route is not a response stream route");
    }
    return dispatch_stream_route(
        request, resolved, memory, resolved.stream_handler(), services.with_response_stream(response_stream));
}

task<std::optional<http_response>> detail::route_table::dispatch_websocket(const http_request& request,
    const resolved_route& resolved, request_memory& memory, const route_stream_handler_type& handler,
    context_services services) const {
    if (resolved.route().endpoint().get_websocket() == nullptr) {
        throw std::logic_error("route is not a websocket route");
    }
    return dispatch_stream_route(request, resolved, memory, handler, services);
}

task<std::optional<http_response>> detail::route_table::dispatch_tunnel(const http_request& request,
    const resolved_route& resolved, request_memory& memory, const route_stream_handler_type& handler,
    context_services services) const {
    if (resolved.route().endpoint().tunnel() == nullptr) {
        throw std::logic_error("route is not a CONNECT tunnel route");
    }
    return dispatch_stream_route(request, resolved, memory, handler, services);
}

task<http_response> detail::route_table::dispatch(const http_request& request,
    const route_resolution& resolution, request_memory& memory, context_services services) const {
    co_return co_await dispatch_request(request, resolution, memory, services,
        document_root_binding::none(), dispatch_failure_type::propagate,
        static_file_selection_mode::identity_only);
}

task<http_response> detail::route_table::dispatch_request(const http_request& request,
    const route_resolution& resolution, request_memory& memory, context_services services,
    document_root_binding document_root, dispatch_failure_type failure,
    static_file_selection_mode static_file_mode) const {
    std::exception_ptr dispatch_exception;
    try {
        const auto* resolved = resolution.resolved();
        if (resolved == nullptr) {
            // A document-root file or an automatic OPTIONS answer: complete
            // responses that still pass through the unmatched-request chain,
            // like every other answer to a request no route matched. A
            // rejected target is answered by the not-found path alone.
            auto unmatched_response = resolution.rejected() != nullptr
                                          ? std::nullopt
                                          : select_document_root_fallback(
                                                document_root, request, memory, services, static_file_mode);
            // One handle_error co_await serves both rejection kinds: each
            // co_await expression reserves its own slots for the call's
            // temporaries in the frame, so distinct inline sites would each
            // add a resident http_response-sized block.
            std::optional<http_error_info> error;
            std::uint32_t allowed_methods = 0;
            // Room for the extension tokens a 405 must name in Allow. Fixed and
            // small: a resource carrying more distinct extension methods than
            // this would be pathological, and truncating beats allocating on
            // the error path.
            std::string_view extension_method_buffer[8];
            std::span<const std::string_view> extension_methods;
            if (unmatched_response) {
                // Served from the document root.
            } else if (request.known_method() == http_known_method::unknown &&
                       !recognizes_method_token(request.method())) {
                // RFC 9110 15.5.6/15.6.2: 405 says the method is known here but
                // unsupported by this resource; a token no route registered is
                // not known here at all, so it stays 501 whatever the path holds.
                error = http_error_info({.status_ = ruvia::http_status::not_implemented,
                    .message_ = "method not implemented"});
            } else if (request.known_method() == http_known_method::unknown) {
                if (const auto* method_not_allowed = resolution.method_not_allowed()) {
                    error = http_error_info({.status_ = ruvia::http_status::method_not_allowed,
                        .message_ = "method not allowed"});
                    allowed_methods = method_not_allowed->allowed_methods();
                    extension_methods = extension_methods_for(request.path(), extension_method_buffer);
                }
                // Otherwise the method is known here but this path has nothing:
                // an ordinary 404, which the fallback below produces.
            } else if (request.known_method() == http_known_method::options &&
                       request.path() == "*") {
                // RFC 9110 9.3.7: OPTIONS * asks about the server in general.
                // Methods differ per resource, so a server-wide Allow would
                // advertise methods most resources reject; answer without one.
                unmatched_response.emplace(http_response({.resource_ = memory.resource()}));
                unmatched_response->status(ruvia::http_status::no_content);
            } else if (const auto* method_not_allowed = resolution.method_not_allowed()) {
                extension_methods = extension_methods_for(request.path(), extension_method_buffer);
                if (request.known_method() == http_known_method::options) {
                    unmatched_response.emplace(make_allow_no_content_response(
                        memory, method_not_allowed->allowed_methods(), extension_methods));
                } else {
                    error = http_error_info({.status_ = ruvia::http_status::method_not_allowed,
                        .message_ = "method not allowed"});
                    allowed_methods = method_not_allowed->allowed_methods();
                }
            }

            if (unmatched_response) {
                auto context_value = detail::context_access::make(memory, request, services.with_routes(*this));
                auto terminal = [&unmatched_response](context&) -> task<http_response> {
                    co_return std::move(*unmatched_response);
                };
                co_return co_await run_unmatched_chain(
                    context_value, callback_access::bind<task<http_response>(context&)>(terminal));
            }

            if (error) {
                auto response = co_await handle_error(request, memory, *error, services);
                if (allowed_methods != 0 || !extension_methods.empty()) {
                    response.allow_methods(allowed_methods, extension_methods);
                }
                co_return std::move(response);
            }

            co_return co_await handle_not_found(request, memory, services);
        }

        auto context_value = make_route_context(memory, request, *resolved, services.with_routes(*this));
        std::exception_ptr exception;
        try {
            co_return co_await invoke_route(*resolved, context_value);
        } catch (...) {
            exception = std::current_exception();
        }
        co_return co_await handle_exception(context_value, exception);
    } catch (...) {
        if (failure == dispatch_failure_type::propagate) {
            throw;
        }
        dispatch_exception = std::current_exception();
    }
    co_return co_await handle_exception(request, memory, dispatch_exception, services);
}

task<http_response> detail::route_table::dispatch_buffered_response(const http_request& request,
    const route_resolution& resolution, request_memory& memory, document_root_binding document_root,
    context_services services, static_file_selection_mode static_file_mode) const {
    // Plain forwarding, not a coroutine: document-root selection and the
    // failure policy live in the existing dispatch frame, so the unified
    // application entry adds no request-path allocation.
    return dispatch_request(request, resolution, memory, services, std::move(document_root),
        dispatch_failure_type::respond, static_file_mode);
}

}  // namespace ruvia
