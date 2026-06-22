#pragma once

namespace ruvia {

template <std::size_t N>
inline HttpResponse Context::text(
    const char (&body)[N],
    std::uint16_t statusCode,
    std::string_view statusText) const {
    const auto size = N > 0 && body[N - 1] == '\0' ? N - 1 : N;
    return textStaticView(std::string_view(body, size), statusCode, statusText);
}

}  // namespace ruvia
