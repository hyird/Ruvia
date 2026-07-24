#pragma once

// Inline definitions for the public Context API.

namespace ruvia {

template <std::size_t N>
inline HttpResponse Context::body(const char (&value)[N]) const {
    const auto size = N > 0 && value[N - 1] == '\0' ? N - 1 : N;
    return bodyStaticView(std::string_view(value, size));
}

template <std::size_t N>
inline HttpResponse Context::text(const char (&body)[N]) const {
    const auto size = N > 0 && body[N - 1] == '\0' ? N - 1 : N;
    return textStaticView(std::string_view(body, size));
}

template <std::size_t N>
inline HttpResponse Context::html(const char (&body)[N]) const {
    const auto size = N > 0 && body[N - 1] == '\0' ? N - 1 : N;
    return htmlStaticView(std::string_view(body, size));
}

template <typename Fn>
inline Task<BlockingResult<std::invoke_result_t<Fn&>>>
Context::tryRunBlocking(Fn fn) const {
    // Not a coroutine: a missing pool is a configuration mistake and surfaces
    // at the call, not at the first co_await. Copying the worker handle is the
    // one ownership share this path takes -- the result may outlive the request
    // that asked for it, and offloading already costs a thread hop.
    return ruvia::runBlocking(blockingPool(), worker_, std::move(fn));
}

template <typename Rep, typename Period, typename Fn>
inline Task<BlockingResult<std::invoke_result_t<Fn&>>>
Context::tryRunBlocking(std::chrono::duration<Rep, Period> timeout, Fn fn) const {
    return ruvia::runBlocking(blockingPool(), worker_, timeout, std::move(fn));
}

template <typename Fn>
inline Task<std::invoke_result_t<Fn&>> Context::runBlocking(Fn fn) const {
    auto result = co_await tryRunBlocking(std::move(fn));
    if constexpr (std::is_void_v<std::invoke_result_t<Fn&>>) {
        std::move(result).value();
        co_return;
    } else {
        co_return std::move(result).value();
    }
}

template <typename Rep, typename Period, typename Fn>
inline Task<std::invoke_result_t<Fn&>> Context::runBlocking(
    std::chrono::duration<Rep, Period> timeout,
    Fn fn) const {
    auto result = co_await tryRunBlocking(timeout, std::move(fn));
    if constexpr (std::is_void_v<std::invoke_result_t<Fn&>>) {
        std::move(result).value();
        co_return;
    } else {
        co_return std::move(result).value();
    }
}

}  // namespace ruvia
