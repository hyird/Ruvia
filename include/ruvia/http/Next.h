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

    [[nodiscard]] Task<void> operator()() const;

private:
    friend struct detail::NextAccess;

    constexpr Next(Context& context, void* target, Invoke invoke) noexcept
        : context_(&context),
          callable_(target, invoke) {}

    Context* context_{nullptr};
    detail::CallableRef<void, Context&> callable_;
};

}  // namespace ruvia
