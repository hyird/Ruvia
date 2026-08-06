#pragma once

#include <concepts>
#include <cstddef>
#include <type_traits>
#include <utility>

#include "ruvia/core/Task.h"
#include "ruvia/core/memory/PmrObject.h"
#include "ruvia/core/memory/ProcessResource.h"
#include "ruvia/http/HttpResponse.h"
#include "ruvia/web/Error.h"

namespace ruvia {

class Context;

namespace detail {

// Fallback handlers are registered once at startup and then read on the error
// path of every worker, so the callable is copied onto the process resource and
// the handler value stays two pointers: trivially copyable, cheap to pass by
// value, and outliving every router that holds it.
template <typename Callable, typename... Args>
[[nodiscard]] void* storeFallbackHandler(Callable&& callable) {
    return constructPmrObject<std::decay_t<Callable>>(processResource(), std::forward<Callable>(callable));
}

}  // namespace detail

// Answers a request that failed. Accepts a plain function -- onError(&handler)
// -- and equally a lambda or any other callable, including one that captures
// the logger, config, or metrics sink the handler needs. The callable is copied
// at registration and must stay valid for the process, so it must not capture
// anything request-scoped by reference.
class HttpErrorHandler final {
public:
    constexpr HttpErrorHandler() noexcept = default;

    constexpr HttpErrorHandler(std::nullptr_t) noexcept {}

    template <typename Callable, typename Stored = std::decay_t<Callable>>
        requires(!std::is_same_v<Stored, HttpErrorHandler> && std::is_invocable_r_v<Task<HttpResponse>, Stored&, Context&, HttpErrorInfo>)
    HttpErrorHandler(Callable&& callable)
        : target_(detail::storeFallbackHandler(std::forward<Callable>(callable))),
          invoke_([](void* target, Context& context, HttpErrorInfo error) -> Task<HttpResponse> { return (*static_cast<Stored*>(target))(context, std::move(error)); }) {}

    [[nodiscard]] Task<HttpResponse> operator()(Context& context, HttpErrorInfo error) const {
        return invoke_(target_, context, std::move(error));
    }

    [[nodiscard]] constexpr explicit operator bool() const noexcept {
        return invoke_ != nullptr;
    }

    [[nodiscard]] friend constexpr bool operator==(const HttpErrorHandler& left, const HttpErrorHandler& right) noexcept {
        return left.target_ == right.target_ && left.invoke_ == right.invoke_;
    }

private:
    using Invoke = Task<HttpResponse> (*)(void*, Context&, HttpErrorInfo);

    void* target_{nullptr};
    Invoke invoke_{nullptr};
};

// Answers a request that matched no route, under the same registration and
// lifetime rules as HttpErrorHandler.
class HttpNotFoundHandler final {
public:
    constexpr HttpNotFoundHandler() noexcept = default;

    constexpr HttpNotFoundHandler(std::nullptr_t) noexcept {}

    template <typename Callable, typename Stored = std::decay_t<Callable>>
        requires(!std::is_same_v<Stored, HttpNotFoundHandler> && std::is_invocable_r_v<Task<HttpResponse>, Stored&, Context&>)
    HttpNotFoundHandler(Callable&& callable)
        : target_(detail::storeFallbackHandler(std::forward<Callable>(callable))),
          invoke_([](void* target, Context& context) -> Task<HttpResponse> { return (*static_cast<Stored*>(target))(context); }) {}

    [[nodiscard]] Task<HttpResponse> operator()(Context& context) const {
        return invoke_(target_, context);
    }

    [[nodiscard]] constexpr explicit operator bool() const noexcept {
        return invoke_ != nullptr;
    }

    [[nodiscard]] friend constexpr bool operator==(const HttpNotFoundHandler& left, const HttpNotFoundHandler& right) noexcept {
        return left.target_ == right.target_ && left.invoke_ == right.invoke_;
    }

private:
    using Invoke = Task<HttpResponse> (*)(void*, Context&);

    void* target_{nullptr};
    Invoke invoke_{nullptr};
};

// Two pointers, so a handler is cheap to copy into every worker's route table.
static_assert(sizeof(HttpErrorHandler) == 2 * sizeof(void*));
static_assert(sizeof(HttpNotFoundHandler) == 2 * sizeof(void*));

}  // namespace ruvia
