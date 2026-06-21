#include "JwtInternal.h"

#include <stdexcept>

namespace ruvia::detail {

bool jwtIsReservedClaim(std::string_view name) noexcept {
    return name == "iss" || name == "sub" || name == "aud" || name == "exp" ||
        name == "nbf" || name == "iat" || name == "jti";
}

std::int64_t jwtEpochSeconds(std::chrono::system_clock::time_point value) {
    return std::chrono::duration_cast<std::chrono::seconds>(value.time_since_epoch()).count();
}

std::chrono::system_clock::time_point jwtFromEpochSeconds(std::int64_t value) {
    return std::chrono::system_clock::time_point(std::chrono::seconds(value));
}

JwtTokenParts jwtSplitToken(std::string_view token) {
    const auto first = token.find('.');
    const auto second = first == std::string_view::npos ? std::string_view::npos : token.find('.', first + 1);
    if (first == std::string_view::npos ||
        second == std::string_view::npos ||
        token.find('.', second + 1) != std::string_view::npos) {
        throw std::invalid_argument("JWT token must have three sections");
    }
    return JwtTokenParts{
        token.substr(0, first),
        token.substr(first + 1, second - first - 1),
        token.substr(second + 1),
        token.substr(0, second)};
}

}  // namespace ruvia::detail
