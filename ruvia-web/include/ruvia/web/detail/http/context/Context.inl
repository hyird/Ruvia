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

}  // namespace ruvia
