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

    constexpr Next() noexcept = default;

    [[nodiscard]] explicit operator bool() const noexcept {
        return invoke_ != nullptr;
    }

    [[nodiscard]] Task<HttpResponse> operator()(Context& context) const;

private:
    friend struct detail::NextAccess;

    constexpr Next(void* target, Invoke invoke) noexcept : target_(target), invoke_(invoke) {}

    void* target_{nullptr};
    Invoke invoke_{nullptr};
};

}  // namespace ruvia
