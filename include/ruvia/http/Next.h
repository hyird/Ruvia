#pragma once

#include "ruvia/http/detail/CallableRef.h"

namespace ruvia {

class Context;

namespace detail {
struct NextAccess;
}  // namespace detail

class Next final {
public:
    using Invoke = detail::CallableRef<void, Context&>::Invoke;

    [[nodiscard]] Task<void> operator()(Context& context) const;

private:
    friend struct detail::NextAccess;

    constexpr Next(void* target, Invoke invoke) noexcept
        : callable_(target, invoke) {}

    detail::CallableRef<void, Context&> callable_;
};

}  // namespace ruvia
