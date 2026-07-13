#pragma once

// Inline definitions for the public Context API.

namespace ruvia {

template <std::size_t N>
inline HttpResponse Context::body(
    const char (&value)[N],
    std::optional<std::uint16_t> statusCode) const {
    const auto size = N > 0 && value[N - 1] == '\0' ? N - 1 : N;
    return body(std::string_view(value, size), statusCode);
}

template <std::size_t N>
inline HttpResponse Context::body(
    const char (&value)[N],
    std::optional<std::uint16_t> statusCode,
    std::span<const HttpHeaderView> headers) const {
    const auto size = N > 0 && value[N - 1] == '\0' ? N - 1 : N;
    return body(std::string_view(value, size), statusCode, headers);
}

template <std::size_t N>
inline HttpResponse Context::body(const char (&value)[N], ResponseInit init) const {
    const auto size = N > 0 && value[N - 1] == '\0' ? N - 1 : N;
    return body(std::string_view(value, size), init);
}

template <std::size_t N>
inline HttpResponse Context::text(
    const char (&body)[N],
    std::optional<std::uint16_t> statusCode) const {
    const auto size = N > 0 && body[N - 1] == '\0' ? N - 1 : N;
    return textStaticView(std::string_view(body, size), statusCode);
}

template <std::size_t N>
inline HttpResponse Context::text(
    const char (&body)[N],
    std::optional<std::uint16_t> statusCode,
    std::span<const HttpHeaderView> headers) const {
    const auto size = N > 0 && body[N - 1] == '\0' ? N - 1 : N;
    return text(std::string_view(body, size), statusCode, headers);
}

template <std::size_t N>
inline HttpResponse Context::text(const char (&body)[N], ResponseInit init) const {
    const auto size = N > 0 && body[N - 1] == '\0' ? N - 1 : N;
    return text(std::string_view(body, size), init);
}

template <std::size_t N>
inline HttpResponse Context::html(
    const char (&body)[N],
    std::optional<std::uint16_t> statusCode) const {
    const auto size = N > 0 && body[N - 1] == '\0' ? N - 1 : N;
    return html(std::string_view(body, size), statusCode);
}

template <std::size_t N>
inline HttpResponse Context::html(
    const char (&body)[N],
    std::optional<std::uint16_t> statusCode,
    std::span<const HttpHeaderView> headers) const {
    const auto size = N > 0 && body[N - 1] == '\0' ? N - 1 : N;
    return html(std::string_view(body, size), statusCode, headers);
}

template <std::size_t N>
inline HttpResponse Context::html(const char (&body)[N], ResponseInit init) const {
    const auto size = N > 0 && body[N - 1] == '\0' ? N - 1 : N;
    return html(std::string_view(body, size), init);
}

}  // namespace ruvia
