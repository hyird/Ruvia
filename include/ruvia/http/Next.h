#pragma once

#include "ruvia/http/detail/CallableRef.h"

namespace ruvia {

class Context;
class HttpResponse;

namespace detail {
struct NextAccess;
}  // namespace detail

class Next final {
public:
    using Invoke = detail::CallableRef<HttpResponse, Context&>::Invoke;

    [[nodiscard]] Task<HttpResponse> operator()(Context& context) const;

private:
    friend struct detail::NextAccess;

    constexpr Next(void* target, Invoke invoke) noexcept
        : callable_(target, invoke) {}

    detail::CallableRef<HttpResponse, Context&> callable_;
};

}  // namespace ruvia
