#include <exception>
#include <memory>
#include <stdexcept>
#include <utility>

#include "ruvia/web/error.h"

#include "context/context_access.h"
#include "http/http_error_response.h"
#include "http/session_access.h"
#include "router/route_stream_state.h"
#include "router/route_table.h"
#include "server/http_response_stream_state.h"

namespace ruvia {

namespace {

void store_repeated_next_error(context& context_value) {
    detail::context_access::set_error(
        context_value, std::make_exception_ptr(std::logic_error("next() called multiple times")));
    detail::context_access::set_response(
        context_value, detail::make_default_error_response(context_value.arena(),
                           http_error_info({.status_ = ruvia::http_status::internal_server_error,
                               .code_ = "next_called_multiple_times",
                               .message_ = "next() called multiple times"})));
}

void store_next_after_response_error(context& context_value) {
    detail::context_access::set_error(
        context_value, std::make_exception_ptr(std::logic_error("next() called after respond()")));
    detail::context_access::set_response(
        context_value, detail::make_default_error_response(context_value.arena(),
                           http_error_info({.status_ = ruvia::http_status::internal_server_error,
                               .code_ = "next_called_after_response",
                               .message_ = "next() called after respond()"})));
}

[[nodiscard]] bool validate_next_invocation(detail::next_state& state_value) {
    auto& context_value = *state_value.context_;
    if (state_value.invocation_ != detail::next_state::invocation_type::ready) {
        store_repeated_next_error(context_value);
        return false;
    }
    if (detail::context_access::has_response(context_value)) {
        store_next_after_response_error(context_value);
        return false;
    }
    return true;
}

detail::next_state::control_type* make_next_control(context& context_value) {
    auto* control = static_cast<detail::next_state::control_type*>(context_value.arena()->allocate(
        sizeof(detail::next_state::control_type), alignof(detail::next_state::control_type)));
    std::construct_at(control);
    return control;
}

class next_control_scope final {
public:
    explicit next_control_scope(detail::next_state::control_type& control) noexcept
        : control_(&control) {}

    next_control_scope(const next_control_scope&) = delete;
    next_control_scope& operator=(const next_control_scope&) = delete;

    ~next_control_scope() {
        control_->expire();
    }

private:
    detail::next_state::control_type* control_;
};

next_control_scope make_next_control_scope(detail::next_state::control_type& control) noexcept {
    return next_control_scope(control);
}

}  // namespace

task<http_response> detail::route_table::invoke_route(
    const route_entry& route, context& context_value) const {
    const auto* endpoint = route.endpoint().buffered();
    if (endpoint == nullptr) {
        throw std::logic_error("route is not a buffered-response route");
    }
    // Hot path: a route with no middleware goes straight to the handler. The
    // context response slot is only needed when middleware can observe the
    // downstream response through context::response().
    if (!route.has_middleware()) {
        return endpoint->handler()(context_value);
    }
    return invoke_route_with_middleware(route, context_value);
}

task<http_response> detail::route_table::invoke_route_with_middleware(
    const route_entry& route, context& context_value) const {
    co_await invoke_middleware_at(route, 0, context_value);
    if (detail::context_access::has_response(context_value)) {
        co_return detail::context_access::take_response(context_value);
    }
    if (auto exception = context_value.exception()) {
        co_return co_await handle_exception(context_value, exception);
    }
    throw std::logic_error(
        "context is not finalized; middleware must set a response or await next()");
}

task<void> detail::route_table::invoke_middleware_at(
    const route_entry& route, std::size_t index, context& context_value) const {
    if (index >= route.middleware_count()) {
        const auto* endpoint = route.endpoint().buffered();
        if (endpoint == nullptr) {
            throw std::logic_error("route is not a buffered-response route");
        }
        auto response = co_await endpoint->handler()(context_value);
        detail::context_access::set_response(context_value, std::move(response));
        co_return;
    }

    const auto& middleware_value = middleware_frames_[route.middleware_offset() + index];
    auto& control = *make_next_control(context_value);
    auto control_scope = make_next_control_scope(control);
    auto& next_value = next_access::make_in(context_value.arena(),
        detail::next_state{.table_ = this,
            .route_ = &route,
            .context_ = &context_value,
            .control_ = &control,
            .index_ = index + 1},
        &route_table::invoke_middleware_continuation);
    auto task_value = middleware_value(context_value, next_value);
    co_await std::move(task_value);
    co_return;
}

task<void> detail::route_table::invoke_middleware_continuation(next_state state_value) {
    if (!validate_next_invocation(state_value)) {
        co_return;
    }
    auto* context_value = state_value.context_;
    const auto* table_value = state_value.table_;
    const auto* route = state_value.route_;
    std::exception_ptr exception;
    try {
        co_await table_value->invoke_middleware_at(*route, state_value.index_, *context_value);
    } catch (...) {
        exception = std::current_exception();
    }
    if (exception != nullptr) {
        co_await table_value->store_middleware_exception_response(*context_value, exception);
    }
}

// The unmatched-request chain. It mirrors the route chain exactly except for
// its terminal: there is no route endpoint, so the 404/405/501 response comes
// from the caller-supplied thunk instead.
task<http_response> detail::route_table::run_unmatched_chain(
    context& context_value, const unmatched_terminal_type& terminal) const {
    if (unmatched_middleware_count_ == 0) {
        return terminal(context_value);
    }
    return [](const route_table* table_value, context* unmatched_context,
               const unmatched_terminal_type* unmatched_terminal) -> task<http_response> {
        co_await table_value->invoke_unmatched_middleware_at(0, *unmatched_context, *unmatched_terminal);
        if (detail::context_access::has_response(*unmatched_context)) {
            co_return detail::context_access::take_response(*unmatched_context);
        }
        if (auto exception = unmatched_context->exception()) {
            co_return co_await table_value->handle_exception(*unmatched_context, exception);
        }
        throw std::logic_error(
            "context is not finalized; middleware must set a response or await next()");
    }(this, &context_value, &terminal);
}

task<void> detail::route_table::invoke_unmatched_middleware_at(
    std::size_t index, context& context_value, const unmatched_terminal_type& terminal) const {
    if (index >= unmatched_middleware_count_) {
        auto response = co_await terminal(context_value);
        detail::context_access::set_response(context_value, std::move(response));
        co_return;
    }

    const auto& middleware_value = middleware_frames_[unmatched_middleware_offset_ + index];
    auto& control = *make_next_control(context_value);
    auto control_scope = make_next_control_scope(control);
    auto& next_value = next_access::make_in(context_value.arena(),
        detail::next_state{.table_ = this,
            .context_ = &context_value,
            .unmatched_terminal_ = &terminal,
            .control_ = &control,
            .index_ = index + 1},
        &route_table::invoke_unmatched_middleware_continuation);
    auto task_value = middleware_value(context_value, next_value);
    co_await std::move(task_value);
    co_return;
}

task<void> detail::route_table::invoke_unmatched_middleware_continuation(next_state state_value) {
    if (!validate_next_invocation(state_value)) {
        co_return;
    }
    auto* context_value = state_value.context_;
    const auto* table_value = state_value.table_;
    const auto* terminal =
        static_cast<const route_table::unmatched_terminal_type*>(state_value.unmatched_terminal_);
    std::exception_ptr exception;
    try {
        co_await table_value->invoke_unmatched_middleware_at(state_value.index_, *context_value, *terminal);
    } catch (...) {
        exception = std::current_exception();
    }
    if (exception != nullptr) {
        co_await table_value->store_middleware_exception_response(*context_value, exception);
    }
}

task<void> detail::route_table::invoke_stream_middleware_at(const route_entry& route, std::size_t index,
    context& context_value, stream_middleware_chain_state& chain, const route_stream_handler_type& handler) const {
    if (index >= route.middleware_count()) {
        if (route.endpoint().get_websocket() != nullptr && context_value.try_session()) {
            co_await session_access::commit(context_value);
        }
        chain.mark_handler_invoked();
        co_await handler(context_value);
        co_return;
    }

    const auto& middleware_value = middleware_frames_[route.middleware_offset() + index];
    auto& control = *make_next_control(context_value);
    auto control_scope = make_next_control_scope(control);
    auto& next_value = next_access::make_in(context_value.arena(),
        detail::next_state{.table_ = this,
            .route_ = &route,
            .context_ = &context_value,
            .stream_chain_ = &chain,
            .stream_handler_ = &handler,
            .control_ = &control,
            .index_ = index + 1},
        &route_table::invoke_stream_middleware_continuation);
    auto task_value = middleware_value(context_value, next_value);
    co_await std::move(task_value);
    co_return;
}

task<void> detail::route_table::invoke_stream_middleware_continuation(next_state state_value) {
    if (!validate_next_invocation(state_value)) {
        co_return;
    }
    auto* context_value = state_value.context_;
    const auto* table_value = state_value.table_;
    const auto* route = state_value.route_;
    auto* chain = state_value.stream_chain_;
    std::exception_ptr exception;
    try {
        co_await table_value->invoke_stream_middleware_at(
            *route, state_value.index_, *context_value, *chain, *state_value.stream_handler_);
    } catch (const response_stream_head_only_complete&) {
        // Not a failure: the committed head already completed a
        // body-suppressed message (HEAD on a streaming route). Let the signal
        // unwind through every middleware frame so dispatch_stream_route can
        // finish the stream as a head-only success instead of rendering a
        // buffered error response that can no longer be sent.
        throw;
    } catch (...) {
        exception = std::current_exception();
    }
    if (exception != nullptr) {
        co_await table_value->store_middleware_exception_response(*context_value, exception);
    }
}

task<void> detail::route_table::store_middleware_exception_response(
    context& context_value, std::exception_ptr exception) const {
    auto response = co_await handle_exception(context_value, exception);
    detail::context_access::set_response(context_value, std::move(response));
}

}  // namespace ruvia
