#pragma once

#include <type_traits>
#include <utility>

#include "ruvia/web/context.h"
#include "ruvia/web/middleware.h"
#include "ruvia/web/testing.h"

#include "test_harness.h"

namespace context_request_test {

template <typename callback_type>
class context_callback final : public ruvia::middleware {
public:
    static constexpr bool ruvia_runs_on_unmatched_requests = true;

    explicit context_callback(callback_type* callback) noexcept
        : callback_(callback) {}

    ruvia::task<void> handle(ruvia::context& context_value, ruvia::next&) {
        co_await (*callback_)(context_value);
    }

private:
    callback_type* callback_;
};

template <typename callback_type>
[[nodiscard]] inline ruvia::test_response with_context(
    const ruvia::test_request& request, callback_type&& callback) {
    ruvia::test_app app;
    app.use<context_callback<std::remove_reference_t<callback_type>>>(&callback);
    return app.request(request);
}

}  // namespace context_request_test
