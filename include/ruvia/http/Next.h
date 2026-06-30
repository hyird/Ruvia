#pragma once

#include "ruvia/http/detail/CallableRef.h"

namespace ruvia {

namespace detail {
struct NextAccess;
}  // namespace detail

class Next final {
public:
    using Invoke = detail::CallableRef<void>::Invoke;

    [[nodiscard]] Task<void> operator()() const;

private:
    friend struct detail::NextAccess;

    constexpr Next(void* target, Invoke invoke) noexcept
        : callable_(target, invoke) {}

    detail::CallableRef<void> callable_;
};

}  // namespace ruvia
