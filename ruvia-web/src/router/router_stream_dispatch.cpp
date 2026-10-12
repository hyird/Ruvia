#include <optional>
#include <utility>

#include "ruvia/web/session.h"
#include "ruvia/web/streaming.h"

#include "context/context_access.h"
#include "http/session_access.h"
#include "http/streaming_access.h"
#include "router/route_context.h"
#include "router/route_table.h"
#include "server/http_response_stream_state.h"

// Running a route that streams its response: binding the writer to the context
// for the handler's lifetime, producing the head the HTTP/1 sink commits, and
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

}  // namespace

task<std::optional<http_response>> detail::route_table::dispatch_stream_route(
    const http_request& request, const resolved_route& resolved, request_memory& memory,
    route_stream_handler_type handler, context_services services) const {
    const auto& route = resolved.route();
    auto context_value = make_route_context(memory, request, resolved, services.with_routes(*this));
    const auto* response_stream_output = services.response_output().response_stream();
    response_stream_context_binding stream_context_binding(
        response_stream_output != nullptr ? &response_stream_output->writer() : nullptr);
    if (response_stream_output != nullptr) {
        detail::streaming_access::bind_context(
            response_stream_output->writer(), context_value, context_value.get_stop_token());
    }

    // Records that next() reached the handler even when it emitted no bytes.
    bool handler_invoked = false;
    std::exception_ptr exception;
    try {
        if (!route.has_middleware()) {
            handler_invoked = true;
            co_await handler(context_value);
        } else {
            // The stream terminal: a websocket or tunnel upgrade commits the
            // session the middleware chain opened before its handshake head,
            // then the handler runs. A response stream commits it lazily with
            // its head, so the handler may still change the session first.
            auto terminal = [&handler_invoked, &handler, &route](context& terminal_context) -> task<void> {
                if (route.endpoint().response_stream() == nullptr && terminal_context.try_session()) {
                    co_await session_access::commit(terminal_context);
                }
                handler_invoked = true;
                co_await handler(terminal_context);
            };
            auto chain = route_chain(route);
            chain.terminal_ = callback_access::bind<task<void>(context&)>(terminal);
            chain.failure_ = &route_table::contain_stream_failure;
            co_await run_chain(chain, context_value);
            if (!detail::context_access::has_response(context_value)) {
                if (auto context_exception = context_value.exception()) {
                    std::rethrow_exception(context_exception);
                }
                if (!handler_invoked && !detail::context_access::output_committed(context_value)) {
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
        if (response_stream_output != nullptr &&
            detail::streaming_access::committed(response_stream_output->writer()) &&
            detail::is_head_only_complete(exception)) {
            co_await response_stream_output->writer().end();
            co_return std::nullopt;
        }
        if (detail::context_access::output_committed(context_value)) {
            std::rethrow_exception(exception);
        }
        auto response = co_await handle_exception(context_value, exception);
        co_return std::move(response);
    }

    // The middleware chain converts a handler exception into a buffered error
    // response and records it via context.exception() (contain_failure
    // -> handle_exception -> set_error), so a mid-request failure does not surface as
    // a local exception above. Once the output is committed -- a started stream or
    // a websocket/tunnel handshake -- that buffered response can no longer be sent,
    // and finalizing the stream with a clean terminator would frame a truncated
    // body as complete. Rethrow so the driver closes the connection, exactly as
    // the no-middleware path does above. Middleware may
    // still replace an uncommitted response stream, or a websocket terminal that
    // failed during handshake preparation, with a buffered response after `next()`.
    const bool output_committed = detail::context_access::output_committed(context_value);
    if (output_committed) {
        if (auto context_exception = context_value.exception()) {
            std::rethrow_exception(context_exception);
        }
    } else if (detail::context_access::has_response(context_value)) {
        co_return detail::context_access::release_response(context_value);
    }
    if (handler_invoked || output_committed) {
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
