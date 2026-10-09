#include <optional>
#include <utility>

#include "context/context_access.h"
#include "http/session_access.h"
#include "http/streaming_access.h"
#include "router/route_dispatch_services.h"
#include "router/route_stream_state.h"
#include "router/route_table.h"
#include "server/http_response_stream_state.h"

// Running a route that streams its response: binding the writer to the context
// for the handler's lifetime, producing the head the h1/h2 sinks commit, and
// unwinding when the handler throws after bytes are already on the wire.

namespace ruvia {

namespace {

class response_stream_context_binding final {
public:
    explicit response_stream_context_binding(response_stream_writer* writer) noexcept
        : writer_(writer) {}

    ~response_stream_context_binding() {
        if (writer_ != nullptr) {
            detail::streaming_access::release_context(*writer_);
        }
    }

    response_stream_context_binding(const response_stream_context_binding&) = delete;
    response_stream_context_binding& operator=(const response_stream_context_binding&) = delete;

private:
    response_stream_writer* writer_;
};

// Web-side thunk producing the streaming response head from the bound context. It is
// handed to the http streaming layer (response_stream_writer::bind_context) so the h1/h2
// sinks can build the head at commit without naming context_access (web).
[[nodiscard]] task<http_response> streaming_head_thunk(context& context_value) {
    if (context_value.try_session()) {
        co_await detail::session_access::commit(context_value);
    }
    co_return detail::context_access::streaming_head(context_value);
}

}  // namespace

task<std::optional<http_response>> detail::route_table::dispatch_stream_route(
    const http_request& request, const resolved_route& resolved, request_memory& memory,
    const route_stream_handler_type& handler, context_services services) const {
    const auto& route = resolved.route();
    stream_middleware_chain_state middleware_chain;
    auto context_value = make_route_context(memory, request, resolved,
        with_route_handlers(
            services, *this, error_handler_for(request.path()), not_found_handler_for(request.path())));
    const auto* response_stream_output = services.response_output().response_stream();
    const bool websocket_route = route.endpoint().get_websocket() != nullptr;
    const bool tunnel_route = route.endpoint().tunnel() != nullptr;
    response_stream_context_binding stream_context_binding(
        response_stream_output != nullptr ? &response_stream_output->writer() : nullptr);
    if (response_stream_output != nullptr) {
        detail::streaming_access::bind_context(
            response_stream_output->writer(), context_value, context_value.get_stop_token(), &streaming_head_thunk);
    }

    std::exception_ptr exception;
    try {
        if (!route.has_middleware()) {
            middleware_chain.mark_handler_invoked();
            co_await handler(context_value);
        } else {
            co_await invoke_stream_middleware_at(route, 0, context_value, middleware_chain, handler);
            if (!detail::context_access::has_response(context_value)) {
                if (auto context_exception = context_value.exception()) {
                    std::rethrow_exception(context_exception);
                }
                const bool stream_committed =
                    response_stream_output != nullptr &&
                    detail::streaming_access::committed(response_stream_output->writer());
                if (!middleware_chain.handler_invoked() && !stream_committed) {
                    throw std::logic_error(
                        "context is not finalized; stream middleware must set a response, write "
                        "the stream, or await next()");
                }
            }
        }
    } catch (...) {
        exception = std::current_exception();
    }

    if (exception != nullptr) {
        // Head-only completion is a control signal from the writer, not a
        // failure: the committed head already ended the message, and the
        // handler was merely stopped at its first body write. Finish the stream
        // as a normal head-only success.
        bool head_only_complete = false;
        if (response_stream_output != nullptr &&
            detail::streaming_access::committed(response_stream_output->writer())) {
            try {
                std::rethrow_exception(exception);
            } catch (const detail::response_stream_head_only_complete&) {
                head_only_complete = true;
            } catch (...) {
                // Classification only: `exception` still holds this and the
                // committed-failure path below reports it.
            }
        }
        if (head_only_complete) {
            co_await response_stream_output->writer().end();
            co_return std::nullopt;
        }
        if ((websocket_route && detail::context_access::websocket_handshake_started(context_value)) ||
            (tunnel_route && detail::context_access::tunnel_handshake_started(context_value)) ||
            (response_stream_output != nullptr &&
                detail::streaming_access::committed(response_stream_output->writer()))) {
            std::rethrow_exception(exception);
        }
        auto response = co_await handle_exception(context_value, exception);
        co_return std::move(response);
    }

    // The middleware chain converts a handler exception into a buffered error
    // response and records it via context.exception() (store_middleware_exception_response
    // -> handle_exception -> set_error), so a mid-request failure does not surface as
    // a local exception above. When the stream is already committed (or this is a
    // committed websocket handshake), that buffered response can no longer be sent, and finalizing
    // the stream with a clean terminator would frame a truncated body as complete.
    // Rethrow so the driver aborts (connection close / RST_STREAM), exactly as the
    // no-middleware path does through the committed check above.
    if ((websocket_route && detail::context_access::websocket_handshake_started(context_value)) ||
        (tunnel_route && detail::context_access::tunnel_handshake_started(context_value)) || (response_stream_output != nullptr && detail::streaming_access::committed(response_stream_output->writer()))) {
        if (auto context_exception = context_value.exception()) {
            std::rethrow_exception(context_exception);
        }
    }

    const bool stream_committed = response_stream_output != nullptr &&
                                  detail::streaming_access::committed(response_stream_output->writer());
    const bool handler_invoked = middleware_chain.handler_invoked();
    const bool websocket_handled = (websocket_route && detail::context_access::websocket_handshake_started(context_value)) ||
                                   (tunnel_route && detail::context_access::tunnel_handshake_started(context_value));
    // Middleware may replace an uncommitted response stream with a buffered
    // response after `next()`. A websocket terminal can still fail during
    // handshake preparation; only a started handshake prevents HTTP recovery.
    if (detail::context_access::has_response(context_value) && !stream_committed && !websocket_handled) {
        co_return detail::context_access::take_response(context_value);
    }
    if (handler_invoked || stream_committed) {
        // The bound context is local to this coroutine. Finish a response stream
        // before it is destroyed so an empty/bodyless handler cannot leave the
        // sink with a dangling context* that a higher layer later dereferences.
        // Reaching this common point gives middleware post-processing its full
        // chance to adjust an uncommitted head; an already committed writer no
        // longer needs the context.
        if (response_stream_output != nullptr) {
            co_await response_stream_output->writer().end();
        }
        co_return std::nullopt;
    }
    throw std::logic_error("stream route completed without a response or handled output");
}

}  // namespace ruvia
