#pragma once

#include <exception>
#include <span>
#include <string_view>

#include "ruvia/core/task.h"
#include "ruvia/web/detail/callback_ref.h"
#include "ruvia/web/detail/router/route_handlers.h"

#include "server/http_response_stream_state.h"

namespace ruvia::detail {

class route_table;

// One run of a middleware chain: the frames that may apply, their dispatch-time
// path scopes (empty when every frame is unconditional), the terminal that runs
// once the last applicable frame calls next(), and how a failure escaping the
// rest of the chain is turned into the context's response at a frame's next().
// The caller owns this object for the whole run; next_state points at it.
struct middleware_chain final {
    using terminal_type = callback_ref_type<task<void>(context&)>;
    // May rethrow control signals that must unwind every frame.
    using failure_type = task<void> (*)(const route_table&, context&, std::exception_ptr);

    const route_table* table_{nullptr};
    std::span<const route_middleware_type> frames_;
    std::span<const std::string_view> scopes_;
    terminal_type terminal_;
    failure_type failure_{nullptr};
};

// The only middleware chain walker. Runs `chain` from its first applicable
// frame; a failure of that first frame propagates to the caller. Each next()
// is single-shot: a repeated call, or one after a response was set, records an
// error response instead of re-entering the chain, and a next() outliving its
// frame is ignored.
[[nodiscard]] task<void> run_chain(const middleware_chain& chain, context& context_value);

// Classification only; the caller still owns and dispatches the exception.
[[nodiscard]] inline bool is_head_only_complete(const std::exception_ptr& exception) noexcept {
    try {
        std::rethrow_exception(exception);
    } catch (const response_stream_head_only_complete&) {
        return true;
        // NOLINTNEXTLINE(bugprone-empty-catch)
    } catch (...) {
    }
    return false;
}

}  // namespace ruvia::detail
