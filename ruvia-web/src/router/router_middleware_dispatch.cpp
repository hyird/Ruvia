#include <cstddef>
#include <exception>
#include <memory>
#include <new>
#include <stdexcept>
#include <string_view>
#include <utility>

#include "ruvia/web/detail/router/prefix_fallback.h"
#include "ruvia/web/error.h"

#include "context/context_access.h"
#include "http/http_error_response.h"
#include "router/middleware_chain.h"
#include "router/route_table.h"

namespace ruvia {

namespace detail {

struct next_access final {
    [[nodiscard]] static next& make_at(void* storage, next_state state_value, next_invoke_type invoke) noexcept {
        return *::new (storage) next(state_value, invoke);
    }
};

}  // namespace detail

namespace {

void store_next_misuse_error(context& context_value, const char* message, std::string_view code) {
    detail::context_access::set_error(context_value, std::make_exception_ptr(std::logic_error(message)));
    detail::context_access::set_response(
        context_value, detail::make_default_error_response(context_value.arena(),
                           http_error_info({.status_ = ruvia::http_status::internal_server_error,
                               .code_ = code,
                               .message_ = message})));
}

[[nodiscard]] bool validate_next_invocation(const detail::next_state& state_value) {
    auto& context_value = *state_value.context_;
    if (state_value.invocation_ != detail::next_state::invocation_type::ready) {
        store_next_misuse_error(context_value, "next() called multiple times", "next_called_multiple_times");
        return false;
    }
    if (detail::context_access::has_response(context_value)) {
        store_next_misuse_error(context_value, "next() called after respond()", "next_called_after_response");
        return false;
    }
    return true;
}

task<void> ignore_expired_next(detail::next_state) {
    co_return;
}

// Expires a hop's next() when the frame that received it completes, so a
// stored next() called later is ignored.
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

// One hop's control block and the next() handed to its frame share a single
// request-arena allocation; both outlive the frame so a late next() stays safe.
struct next_hop_layout final {
    static constexpr std::size_t next_offset =
        (sizeof(detail::next_state::control_type) + alignof(next) - 1) / alignof(next) * alignof(next);
    static constexpr std::size_t size = next_offset + sizeof(next);
    static constexpr std::size_t alignment = alignof(next) > alignof(detail::next_state::control_type)
                                                 ? alignof(next)
                                                 : alignof(detail::next_state::control_type);
};

task<void> run_chain_hop(detail::next_state state_value);

[[nodiscard]] std::size_t applicable_frame(
    const detail::middleware_chain& chain, std::size_t index, const context& context_value) noexcept {
    if (chain.scopes_.empty()) {
        return index;
    }
    // The same path the route table resolved this request against.
    const auto path = detail::context_access::request(context_value).path();
    while (index < chain.frames_.size() && !detail::path_is_under_prefix(path, chain.scopes_[index])) {
        ++index;
    }
    return index;
}

// One coroutine per hop: the entry hop (no control) lets failures propagate to
// the dispatcher; every hop reached through next() validates the single-shot
// invocation first and contains failures below it through the chain's policy.
task<void> run_chain_hop(detail::next_state state_value) {
    auto& context_value = *state_value.context_;
    const auto& chain = *state_value.chain_;
    const bool contained = state_value.control_ != nullptr;
    if (contained && !validate_next_invocation(state_value)) {
        co_return;
    }
    std::exception_ptr exception;
    try {
        const auto index = applicable_frame(chain, state_value.index_, context_value);
        if (index >= chain.frames_.size()) {
            co_await chain.terminal_(context_value);
        } else {
            auto* storage = static_cast<std::byte*>(
                context_value.arena()->allocate(next_hop_layout::size, next_hop_layout::alignment));
            auto& control = *std::construct_at(reinterpret_cast<detail::next_state::control_type*>(storage));
            const next_control_scope control_scope(control);
            auto& next_value = detail::next_access::make_at(storage + next_hop_layout::next_offset,
                detail::next_state{
                    .chain_ = &chain, .context_ = &context_value, .control_ = &control, .index_ = index + 1},
                &run_chain_hop);
            co_await chain.frames_[index](context_value, next_value);
        }
    } catch (...) {
        if (!contained) {
            throw;
        }
        exception = std::current_exception();
    }
    if (exception != nullptr) {
        co_await chain.failure_(*chain.table_, context_value, exception);
    }
}

}  // namespace

next::awaitable_type next::operator()() & {
    auto state_value = state_;
    auto* control = state_value.control_;
    state_value.invocation_ =
        control == nullptr ? detail::next_state::invocation_type::expired : control->begin_invocation();
    if (state_value.invocation_ == detail::next_state::invocation_type::expired) {
        return awaitable_type(state_value, &ignore_expired_next);
    }
    return awaitable_type(state_value, invoke_);
}

task<void> detail::run_chain(const middleware_chain& chain, context& context_value) {
    return run_chain_hop(next_state{.chain_ = &chain, .context_ = &context_value});
}

detail::middleware_chain detail::route_table::route_chain(const route_entry& route) const noexcept {
    const auto offset = route.middleware_offset();
    const auto count = route.middleware_count();
    return middleware_chain{.table_ = this,
        .frames_ = std::span<const route_middleware_type>(middleware_frames_).subspan(offset, count),
        .scopes_ = route.has_conditional_middleware()
                       ? std::span<const std::string_view>(plan_->middleware_scopes_).subspan(offset, count)
                       : std::span<const std::string_view>{},
        .terminal_ = {},
        .failure_ = &route_table::contain_failure};
}

task<http_response> detail::route_table::invoke_route(const resolved_route& resolved, context& context_value) const {
    const auto& route = resolved.route();
    if (route.endpoint().buffered() == nullptr) {
        throw std::logic_error("route is not a buffered-response route");
    }
    // Hot path: a route with no middleware goes straight to the handler. The
    // context response slot is only needed when middleware can observe the
    // downstream response through context::response().
    if (!route.has_middleware()) {
        return resolved.handler()(context_value);
    }
    return respond_through_chain(route_chain(route), context_value, resolved.handler());
}

task<http_response> detail::route_table::respond_through_chain(
    middleware_chain chain, context& context_value, route_handler_type terminal) const {
    auto respond = [terminal](context& terminal_context) -> task<void> {
        detail::context_access::set_response(terminal_context, co_await terminal(terminal_context));
    };
    chain.terminal_ = callback_access::bind<task<void>(context&)>(respond);
    co_await run_chain(chain, context_value);
    if (detail::context_access::has_response(context_value)) {
        co_return detail::context_access::release_response(context_value);
    }
    if (auto exception = context_value.exception()) {
        co_return co_await handle_exception(context_value, exception);
    }
    throw std::logic_error("context is not finalized; middleware must set a response or await next()");
}

task<http_response> detail::route_table::run_unmatched_chain(
    context& context_value, route_handler_type terminal) const {
    if (plan_->unmatched_middleware_count_ == 0) {
        return terminal(context_value);
    }
    return respond_through_chain(
        middleware_chain{.table_ = this,
            .frames_ = std::span<const route_middleware_type>(middleware_frames_)
                .subspan(plan_->unmatched_middleware_offset_, plan_->unmatched_middleware_count_),
            .scopes_ = {},
            .terminal_ = {},
            .failure_ = &route_table::contain_failure},
        context_value, terminal);
}

task<void> detail::route_table::contain_failure(
    const route_table& table, context& context_value, std::exception_ptr exception) {
    auto response = co_await table.handle_exception(context_value, exception);
    detail::context_access::set_response(context_value, std::move(response));
}

task<void> detail::route_table::contain_stream_failure(
    const route_table& table, context& context_value, std::exception_ptr exception) {
    // Not a failure: the committed head already completed a body-suppressed
    // message (HEAD on a streaming route). Let the signal unwind through every
    // middleware frame so dispatch_stream_route can finish the stream as a
    // head-only success instead of rendering a buffered error response that
    // can no longer be sent.
    if (detail::is_head_only_complete(exception)) {
        std::rethrow_exception(exception);
    }
    co_await contain_failure(table, context_value, exception);
}

}  // namespace ruvia
