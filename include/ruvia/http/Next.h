#pragma once

#include "ruvia/app/Task.h"

namespace ruvia {

class Context;
class HttpResponse;

namespace detail {
struct NextAccess;
}  // namespace detail

class Next final {
public:
    using Invoke = Task<HttpResponse> (*)(void*, Context&);

    [[nodiscard]] Task<HttpResponse> operator()(Context& context) const;

private:
    friend struct detail::NextAccess;

    constexpr Next(void* target, Invoke invoke) noexcept : target_(target), invoke_(invoke) {}

    void* target_;
    Invoke invoke_;
};

}  // namespace ruvia
